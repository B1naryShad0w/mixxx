#include <gtest/gtest.h>

#include <QDir>
#include <iostream>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "library/djlib/djlibimporter.h"
#include "library/djlib/djlibservice.h"
#include "library/djlib/djlibstore.h"
#include "library/trackset/crate/crate.h"
#include "test/librarytest.h"
#include "track/beats.h"
#include "track/track.h"

namespace {

const QString kAlacStem = QStringLiteral("stems/stem01/sin_ALAC_24bit.stem.mp4");
const QString kAacStem = QStringLiteral("stems/stem01/sin_AAC_256kbps_VBR.stem.mp4");
constexpr int kTimeoutMillis = 10000;

QString packageName(int i) {
    return QStringLiteral("Artist - Title %1 [%2].stem.mp4").arg(i).arg(10000000 + i);
}

QString sidecarName(int i) {
    return QStringLiteral("Artist - Title %1 [%2].djlib.json").arg(i).arg(10000000 + i);
}

QString djlibId(int i) {
    return QStringLiteral("trk_%1").arg(i);
}

QJsonObject sidecarJson(int i, qint64 version, const QJsonArray& crates) {
    return QJsonObject{
            {"schema", 1},
            {"track_id", djlibId(i)},
            {"tidal_id", QString::number(10000000 + i)},
            {"isrc", QJsonValue()},
            {"sidecar_version", version},
            {"package_version", 1},
            {"file", packageName(i)},
            {"sha256", QString(64, QChar('a'))},
            {"crates", crates},
            {"tempo", "constant"},
            {"bpm", 124.0},
            {"bpm_source", "tidal"},
            {"grid", QJsonObject{{"first_beat_s", 0.1234}, {"bpm", 124.0}}},
            {"beats_s", QJsonValue()},
            {"first_downbeat_s", 0.5795},
            {"key", "Am"},
            {"replaygain", QJsonObject{{"gain_db", -9.0}, {"peak", 0.944}}},
            {"cues",
                    QJsonArray{QJsonObject{{"type", "main"}, {"pos_s", 0.5795}},
                            QJsonObject{{"type", "hotcue"},
                                    {"number", 1},
                                    {"pos_s", 0.5795},
                                    {"label", "1st downbeat"}}}},
            {"flags", QJsonArray()},
    };
}

QJsonArray crateRefs(const QStringList& ids) {
    QJsonArray result;
    for (const auto& id : ids) {
        result.append(QJsonObject{{"id", id}, {"name", QString(QStringLiteral("Crate ") + id)}});
    }
    return result;
}

QByteArray cratesJson(const QStringList& ids) {
    return QJsonDocument(QJsonObject{{"schema", 1}, {"crates", crateRefs(ids)}}).toJson();
}

/// Writes like the agent: a temp file next to the target, then one rename.
bool writeByRename(const QString& path, const QByteArray& data) {
    const QString tmp = path + QStringLiteral(".tmp");
    {
        QFile file(tmp);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            return false;
        }
    }
    QFile::remove(path);
    return QFile::rename(tmp, path);
}

bool copyByRename(const QString& source, const QString& path) {
    const QString tmp = path + QStringLiteral(".tmp");
    QFile::remove(tmp);
    return QFile::copy(source, tmp) && QFile::rename(tmp, path);
}

} // namespace

class DjlibServiceTest : public LibraryTest {
  protected:
    DjlibServiceTest() {
        EXPECT_TRUE(m_libDir.isValid());
        EXPECT_TRUE(QDir(m_libDir.path()).mkpath(QStringLiteral("tracks")));
        EXPECT_TRUE(QDir(m_libDir.path()).mkpath(QStringLiteral("_djlib")));
    }

    ~DjlibServiceTest() override {
        m_pService.reset();
    }

    void startService() {
        DjlibService::Timing timing;
        timing.debounceMillis = 100;
        timing.maxDelayMillis = 500;
        timing.safetyPassMillis = 60000;
        m_pService = std::make_unique<DjlibService>(
                config(), trackCollectionManager(), nullptr, timing);
        m_pService->setTrackLoadedFn([this](const TrackPointer& pTrack) {
            return pTrack && m_loaded.contains(pTrack->getId());
        });
        m_pService->setLibraryPath(m_libDir.path());
    }

    QString tracksPath(const QString& name) const {
        return QDir(m_libDir.path()).filePath(QStringLiteral("tracks/") + name);
    }

    QString cratesPath() const {
        return QDir(m_libDir.path()).filePath(QStringLiteral("_djlib/crates.json"));
    }

    void writeCrates(const QStringList& ids) {
        ASSERT_TRUE(writeByRename(cratesPath(), cratesJson(ids)));
    }

    void writePackage(int i, const QString& stem = kAlacStem) {
        ASSERT_TRUE(copyByRename(getTestDir().filePath(stem), tracksPath(packageName(i))));
    }

    void writeSidecar(int i, qint64 version, const QStringList& crateIds) {
        ASSERT_TRUE(writeByRename(tracksPath(sidecarName(i)),
                QJsonDocument(sidecarJson(i, version, crateRefs(crateIds))).toJson()));
    }

    TrackPointer track(int i) const {
        return trackByPath(tracksPath(packageName(i)));
    }

    TrackPointer trackByPath(const QString& path) const {
        return trackCollectionManager()->getTrackByRef(TrackRef::fromFilePath(path));
    }

    bool hasDjlibBeats(int i) const {
        const auto pTrack = track(i);
        return pTrack && pTrack->getBeats() &&
                pTrack->getBeats()->getSubVersion() == DjlibImporter::kBeatsSubVersion;
    }

    QStringList managedCrates(int i) {
        QStringList result;
        const auto pTrack = track(i);
        if (!pTrack) {
            return result;
        }
        auto select = internalCollection()->crates().selectTrackCratesSorted(pTrack->getId());
        while (select.next()) {
            const auto mapping = m_pService->store().crateMappingByCrateId(select.crateId());
            if (mapping) {
                result << mapping->djlibId;
            }
        }
        result.sort();
        return result;
    }

    bool waitIdle() {
        // Wait until a pass found nothing more to do.
        return QTest::qWaitFor([this] { return m_pService->status().queued == 0; },
                kTimeoutMillis);
    }

    QTemporaryDir m_libDir;
    QSet<TrackId> m_loaded;
    std::unique_ptr<DjlibService> m_pService;
};

TEST_F(DjlibServiceTest, packageThenSidecarImportsTrack) {
    writeCrates({"crate_a"});
    startService();

    writePackage(1);
    writeSidecar(1, 1, {"crate_a", "crate_other"});

    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    const auto pTrack = track(1);
    EXPECT_TRUE(pTrack->isBpmLocked());
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));
    EXPECT_EQ(mixxx::track::io::key::A_MINOR, pTrack->getKey());

    const auto state = m_pService->store().trackState(djlibId(1));
    ASSERT_TRUE(state);
    EXPECT_EQ(1, state->sidecarVersion);
    EXPECT_EQ(pTrack->getId(), state->trackId);
    EXPECT_FALSE(state->pendingJson);

    // <lib>/tracks is a library root.
    bool rootFound = false;
    for (const auto& root : internalCollection()->loadRootDirs()) {
        rootFound |= root.location() == QFileInfo(tracksPath(QString())).absolutePath() ||
                root.location() == QDir(tracksPath(QString())).absolutePath();
    }
    EXPECT_TRUE(rootFound);
}

TEST_F(DjlibServiceTest, sidecarBeforePackageIsPending) {
    writeCrates({"crate_a"});
    startService();

    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return m_pService->status().waitingForPackage == 1; },
            kTimeoutMillis));
    EXPECT_FALSE(track(1));

    writePackage(1);
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));
    EXPECT_EQ(0, m_pService->status().waitingForPackage);
}

TEST_F(DjlibServiceTest, existingFilesAreImportedAtStart) {
    writeCrates({"crate_a"});
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    startService();
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));
}

TEST_F(DjlibServiceTest, tmpFilesAreIgnored) {
    writeCrates({"crate_a"});
    startService();

    // Names containing ".tmp" anywhere, as left over by an interrupted agent.
    ASSERT_TRUE(QFile::copy(getTestDir().filePath(kAlacStem),
            tracksPath(packageName(1) + QStringLiteral(".tmp"))));
    ASSERT_TRUE(QFile::copy(getTestDir().filePath(kAlacStem),
            tracksPath(QStringLiteral("x.tmp1.stem.mp4"))));
    {
        QFile file(tracksPath(sidecarName(1) + QStringLiteral(".tmp")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(sidecarJson(1, 1, crateRefs({"crate_a"}))).toJson());
    }
    // A real track as a marker that the pass ran.
    writePackage(2);
    writeSidecar(2, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(2); }, kTimeoutMillis));
    m_pService->requestPass();
    ASSERT_TRUE(waitIdle());
    QTest::qWait(300);

    EXPECT_FALSE(m_pService->store().trackState(djlibId(1)));
    EXPECT_FALSE(trackByPath(tracksPath(packageName(1) + QStringLiteral(".tmp"))));
    EXPECT_FALSE(trackByPath(tracksPath(QStringLiteral("x.tmp1.stem.mp4"))));
}

TEST_F(DjlibServiceTest, burstOf200SidecarsIsAppliedOnceEach) {
    constexpr int kCount = 200;
    writeCrates({"crate_a"});
    for (int i = 0; i < kCount; ++i) {
        writePackage(i, kAacStem);
    }
    startService();
    // All packages imported (and probed) first.
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return track(kCount - 1) && m_pService->status().queued == 0; },
            60000));

    QHash<QString, int> applied;
    QObject::connect(m_pService.get(),
            &DjlibService::sidecarApplied,
            m_pService.get(),
            [&applied](const QString& id, qint64) { applied[id]++; });

    // Measure GUI-thread stalls with a 5 ms ticker.
    QElapsedTimer sinceTick;
    qint64 maxGap = 0;
    QTimer ticker;
    ticker.setInterval(5);
    QObject::connect(&ticker, &QTimer::timeout, &ticker, [&] {
        maxGap = std::max(maxGap, sinceTick.restart());
    });
    sinceTick.start();
    ticker.start();

    for (int i = 0; i < kCount; ++i) {
        writeSidecar(i, 1, {"crate_a"});
    }
    const bool allApplied = QTest::qWaitFor(
            [&applied] { return applied.size() == kCount; }, 60000);
    ticker.stop();
    ASSERT_TRUE(allApplied) << applied.size();

    // Further passes find nothing new.
    m_pService->requestPass();
    QTest::qWait(1000);
    ASSERT_TRUE(waitIdle());
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(1, applied.value(djlibId(i))) << i;
    }
    EXPECT_TRUE(hasDjlibBeats(0));
    EXPECT_TRUE(hasDjlibBeats(kCount - 1));
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(kCount - 1));
    std::cout << "[   INFO   ] max GUI-thread gap during the burst: " << maxGap << " ms" << std::endl;
    EXPECT_LT(maxGap, 500);
}

TEST_F(DjlibServiceTest, cratesJsonChangeReappliesMembership) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a", "crate_b"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));

    // Subscribed to crate_b too: membership from the sidecar on disk.
    writeCrates({"crate_a", "crate_b"});
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return managedCrates(1) == QStringList({"crate_a", "crate_b"}); },
            kTimeoutMillis));

    // Unsubscribed from crate_a: crate deleted, track kept.
    writeCrates({"crate_b"});
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return managedCrates(1) == QStringList({"crate_b"}); },
            kTimeoutMillis));
    EXPECT_FALSE(m_pService->store().crateMapping("crate_a"));
    EXPECT_TRUE(track(1));
}

TEST_F(DjlibServiceTest, invalidCratesJsonKeepsCrates) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));

    ASSERT_TRUE(writeByRename(cratesPath(), QByteArray("{garbage")));
    m_pService->requestPass();
    QTest::qWait(500);
    ASSERT_TRUE(waitIdle());
    EXPECT_TRUE(m_pService->store().crateMapping("crate_a"));
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));
}

TEST_F(DjlibServiceTest, djDeletedCrateIsRecreatedWithMembers) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    const auto mapping = m_pService->store().crateMapping("crate_a");
    ASSERT_TRUE(mapping);

    // DJ unlocks and deletes the managed crate.
    Crate crate;
    ASSERT_TRUE(internalCollection()->crates().readCrateById(mapping->crateId, &crate));
    crate.setLocked(false);
    ASSERT_TRUE(internalCollection()->updateCrate(crate));
    ASSERT_TRUE(internalCollection()->deleteCrate(mapping->crateId));

    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return managedCrates(1) == QStringList({"crate_a"}); },
            kTimeoutMillis));
    EXPECT_NE(mapping->crateId, m_pService->store().crateMapping("crate_a")->crateId);
}

TEST_F(DjlibServiceTest, packageRemovedDropsCratesKeepsTrack) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    const TrackId trackId = track(1)->getId();

    // The agent moves both files to _removed.
    ASSERT_TRUE(QDir(m_libDir.path()).mkpath(QStringLiteral("_removed")));
    ASSERT_TRUE(QFile::rename(tracksPath(packageName(1)),
            QDir(m_libDir.path()).filePath(QStringLiteral("_removed/") + packageName(1))));
    ASSERT_TRUE(QFile::rename(tracksPath(sidecarName(1)),
            QDir(m_libDir.path()).filePath(QStringLiteral("_removed/") + sidecarName(1))));

    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return managedCrates(1).isEmpty(); }, kTimeoutMillis));
    EXPECT_TRUE(trackCollectionManager()->getTrackById(trackId));
    EXPECT_TRUE(m_pService->store().crateMapping("crate_a"));
}

TEST_F(DjlibServiceTest, loadedTrackDefersBeatsUntilUnloaded) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return track(1) && track(1)->getSampleRate().isValid(); },
            kTimeoutMillis));
    TrackPointer pTrack = track(1);
    m_loaded.insert(pTrack->getId());

    QSignalSpy applied(m_pService.get(), &DjlibService::sidecarApplied);
    writeSidecar(1, 1, {"crate_a"});
    // Crates apply at once, beats wait for the deck.
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return managedCrates(1) == QStringList({"crate_a"}); },
            kTimeoutMillis));
    QTest::qWait(300);
    EXPECT_FALSE(hasDjlibBeats(1));
    EXPECT_EQ(0, applied.count());
    EXPECT_EQ(1, m_pService->status().deferred);

    m_loaded.clear();
    m_pService->slotDeckTrackChanged(QStringLiteral("[Channel1]"), TrackPointer(), pTrack);
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    EXPECT_EQ(1, applied.count());
    EXPECT_EQ(0, m_pService->status().deferred);
}

TEST_F(DjlibServiceTest, purgedTrackDropsState) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    const TrackRef ref = TrackRef::fromFilePath(tracksPath(packageName(1)), track(1)->getId());

    trackCollectionManager()->purgeTracks({ref});
    EXPECT_FALSE(m_pService->store().trackState(djlibId(1)));
}

TEST_F(DjlibServiceTest, emptyPathDisables) {
    writeCrates({"crate_a"});
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    startService();
    m_pService->setLibraryPath(QString());
    EXPECT_FALSE(m_pService->status().watching);
    QTest::qWait(500);
    EXPECT_EQ(QString(), config()->getValueString(DjlibService::kLibraryPathKey));
    EXPECT_FALSE(track(1));
}

TEST_F(DjlibServiceTest, hiddenTrackKeepsState) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));

    // tracksRemoved is also emitted for hidden tracks: only a purge drops state.
    ASSERT_TRUE(trackCollectionManager()->hideTracks({track(1)->getId()}));
    EXPECT_TRUE(m_pService->store().trackState(djlibId(1)));
}
