#include <gtest/gtest.h>

#include <QSqlQuery>
#include <QDir>
#include <atomic>
#include <memory>
#include <iostream>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "library/djlib/djlibimporter.h"
#include "library/djlib/djlibprotect.h"
#include "library/djlib/djlibservice.h"
#include "library/djlib/djlibstore.h"
#include "library/djlib/dlgprefdjlib.h"
#include "library/library_prefs.h"
#include "library/trackset/crate/crate.h"
#include "proto/keys.pb.h" // needed by keydetectionsettings.h
#include "preferences/keydetectionsettings.h"
#include "test/librarytest.h"
#include "track/beats.h"
#include "track/cue.h"
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
    if (!QFile::copy(source, tmp)) {
        return false;
    }
    QFile::remove(path);
    return QFile::rename(tmp, path);
}

} // namespace

class DjlibServiceTest : public LibraryTest {
  protected:
    DjlibServiceTest() {
        djlib::resetProtectedRootsForTesting();
        EXPECT_TRUE(m_libDir.isValid());
        EXPECT_TRUE(QDir(m_libDir.path()).mkpath(QStringLiteral("tracks")));
        EXPECT_TRUE(QDir(m_libDir.path()).mkpath(QStringLiteral("_djlib")));
    }

    ~DjlibServiceTest() override {
        m_pService.reset();
        djlib::setReadFailureForTesting(nullptr);
        djlib::setWriteFailureForTesting(nullptr);
        // Protection lasts as long as the process: not into other tests.
        djlib::resetProtectedRootsForTesting();
        djlib::setPathCaseSensitivityForTesting(std::nullopt);
    }

    void createService() {
        DjlibService::Timing timing;
        timing.debounceMillis = 100;
        timing.maxDelayMillis = 500;
        timing.safetyPassMillis = m_safetyPassMillis;
        timing.retryMinMillis = 50;
        timing.retryMaxMillis = 400;
        timing.emptyConfirmMillis = m_emptyConfirmMillis;
        timing.exitWaitMillis = m_exitWaitMillis;
        m_pService = std::make_unique<DjlibService>(
                config(), trackCollectionManager(), nullptr, timing);
        m_pService->setTrackLoadedFn([this](const TrackPointer& pTrack) {
            return pTrack && m_loaded.contains(pTrack->getId());
        });
        // Never the real agent or Music folder of the machine running this.
        m_pService->setDiscoveryPaths(agentFilePath(), musicPath());
    }

    QString agentFilePath() const {
        return QDir(m_homeDir.path()).filePath(QStringLiteral("state/djlib-agent/library.json"));
    }

    QString musicPath() const {
        return QDir(m_homeDir.path()).filePath(QStringLiteral("Music"));
    }

    /// As the agent does on every start and library change.
    void writeAgentFile(const QString& libraryDir) {
        ASSERT_TRUE(QDir().mkpath(QFileInfo(agentFilePath()).absolutePath()));
        const QJsonObject json{{"schema", 1}, {"library_dir", libraryDir}};
        ASSERT_TRUE(writeByRename(agentFilePath(), QJsonDocument(json).toJson()));
    }

    void startService() {
        createService();
        m_pService->setLibraryPath(m_libPath);
    }

    /// Like Mixxx: the path comes from the settings.
    void startServiceFromConfig() {
        config()->setValue(DjlibService::kLibraryPathKey, m_libPath);
        createService();
        m_pService->start();
    }

    /// Mixxx closed.
    void stopService() {
        m_pService.reset();
    }

    QString tracksPath(const QString& name) const {
        return QDir(m_libPath).filePath(QStringLiteral("tracks/") + name);
    }

    QString cratesPath() const {
        return QDir(m_libPath).filePath(QStringLiteral("_djlib/crates.json"));
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
        const auto pTrack = track(i);
        return pTrack ? managedCratesOf(pTrack->getId()) : QStringList();
    }

    QStringList managedCratesOf(TrackId trackId) {
        QStringList result;
        auto select = internalCollection()->crates().selectTrackCratesSorted(trackId);
        while (select.next()) {
            const auto mapping = m_pService->store().crateMappingByCrateId(select.crateId());
            if (mapping) {
                result << mapping->djlibId;
            }
        }
        result.sort();
        return result;
    }

    /// The library entry stored with exactly this location, if any.
    TrackId trackIdAt(const QString& location) {
        QSqlQuery query(internalCollection()->database());
        query.prepare(QStringLiteral(
                "SELECT library.id FROM library INNER JOIN track_locations "
                "ON library.location=track_locations.id "
                "WHERE track_locations.location=:location"));
        query.bindValue(QStringLiteral(":location"), location);
        return query.exec() && query.next() ? TrackId(query.value(0)) : TrackId();
    }

    bool waitIdle() {
        // Wait until a pass found nothing more to do.
        return QTest::qWaitFor([this] { return m_pService->status().queued == 0; },
                kTimeoutMillis);
    }

    QTemporaryDir m_libDir;
    QString m_libPath = m_libDir.path();
    QTemporaryDir m_homeDir; // agent state folder and Music folder
    int m_safetyPassMillis = 60000;
    QSet<TrackId> m_loaded;
    int m_emptyConfirmMillis = 300;
    int m_exitWaitMillis = 3000;
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

    // Temp names ("<target>.tmp<digits>"), as left over by an interrupted agent.
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

TEST_F(DjlibServiceTest, titleContainingTmpIsAnOrdinaryTrack) {
    writeCrates({"crate_a"});
    startService();
    // ".tmp" inside a title is not a temp file.
    const QString package = QStringLiteral("Artist - Song.tmp edit [10000001].stem.mp4");
    const QString sidecar = QStringLiteral("Artist - Song.tmp edit [10000001].djlib.json");
    ASSERT_TRUE(copyByRename(getTestDir().filePath(kAlacStem), tracksPath(package)));
    auto json = sidecarJson(1, 1, crateRefs({"crate_a"}));
    json["file"] = package;
    ASSERT_TRUE(writeByRename(tracksPath(sidecar), QJsonDocument(json).toJson()));
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                const auto state = m_pService->store().trackState(djlibId(1));
                return state && state->sidecarVersion == 1;
            },
            kTimeoutMillis));
    const auto pTrack = trackByPath(tracksPath(package));
    ASSERT_TRUE(pTrack);
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(pTrack->getId()));
}

TEST_F(DjlibServiceTest, hugeSidecarIsRejectedNotRead) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    {
        // A valid sidecar padded far beyond the size limit.
        QFile file(tracksPath(sidecarName(1)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        QByteArray json = QJsonDocument(sidecarJson(1, 1, crateRefs({"crate_a"}))).toJson();
        json.append(QByteArray(djlib::kMaxJsonBytes, ' '));
        file.write(json);
    }
    writePackage(2);
    writeSidecar(2, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(2); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    EXPECT_FALSE(m_pService->store().trackState(djlibId(1)));
    EXPECT_FALSE(hasDjlibBeats(1));
}

TEST_F(DjlibServiceTest, passThatThrowsIsRetried) {
    writeCrates({"crate_a"});
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    auto pThrown = std::make_shared<std::atomic<int>>(0);
    djlib::setReadFailureForTesting([pThrown](const QString& path) -> bool {
        if (path.endsWith(sidecarName(1)) && pThrown->fetch_add(1) < 1) {
            throw std::bad_alloc();
        }
        return false;
    });
    startService();
    // The failed pass must not leave the service waiting for it forever.
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    EXPECT_GE(pThrown->load(), 2);
    ASSERT_TRUE(waitIdle());
}

TEST_F(DjlibServiceTest, rewriteWithSameSizeAndTimeIsNoticed) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());

    // Rewritten within the timestamp granularity: same size, same time.
    const QString path = tracksPath(sidecarName(1));
    const QFileInfo before(path);
    const QDateTime mtime = before.lastModified();
    auto json = sidecarJson(1, 2, crateRefs({"crate_a"}));
    json["grid"] = QJsonObject{{"first_beat_s", 0.1234}, {"bpm", 126.0}};
    const QByteArray data = QJsonDocument(json).toJson();
    ASSERT_EQ(before.size(), data.size());
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(data.size(), file.write(data));
        file.flush();
        ASSERT_TRUE(file.setFileTime(mtime, QFileDevice::FileModificationTime));
    }
    ASSERT_EQ(mtime, QFileInfo(path).lastModified());
    m_pService->requestPass();
    ASSERT_TRUE(QTest::qWaitFor(
            [this] {
                const auto state = m_pService->store().trackState(djlibId(1));
                return state && state->sidecarVersion == 2;
            },
            kTimeoutMillis));
    EXPECT_NEAR(126.0, track(1)->getBpm(), 1e-6);
}

TEST_F(DjlibServiceTest, queueRestsDuringLibraryScan) {
    writeCrates({"crate_a"});
    startService();
    ASSERT_TRUE(waitIdle());

    emit trackCollectionManager()->libraryScanStarted();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    // The pass runs, its results wait.
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return m_pService->status().queued > 0; }, kTimeoutMillis));
    QTest::qWait(500);
    EXPECT_FALSE(track(1));
    EXPECT_FALSE(m_pService->store().trackState(djlibId(1)));
    EXPECT_GT(m_pService->status().queued, 0);

    emit trackCollectionManager()->libraryScanFinished();
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));
    EXPECT_EQ(1, m_pService->store().trackState(djlibId(1))->sidecarVersion);
}

TEST_F(DjlibServiceTest, stateIsStoredOnlyAfterTheTrackIsSaved) {
    writeCrates({"crate_a"});
    auto pFail = std::make_shared<bool>(true);
    auto pFailures = std::make_shared<int>(0);
    djlib::setWriteFailureForTesting([pFail, pFailures](const QString& what) {
        if (what == QStringLiteral("save") && *pFail) {
            ++*pFailures;
            return true;
        }
        return false;
    });
    startService();
    QSignalSpy applied(m_pService.get(), &DjlibService::sidecarApplied);
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    // Applied in memory, but the save keeps failing: no fingerprints may be
    // stored, and the write stays queued.
    ASSERT_TRUE(QTest::qWaitFor([pFailures] { return *pFailures >= 3; }, kTimeoutMillis));
    EXPECT_TRUE(hasDjlibBeats(1));
    EXPECT_FALSE(m_pService->store().trackState(djlibId(1)));
    EXPECT_GT(m_pService->status().queued, 0);
    EXPECT_EQ(0, applied.count());

    // A newer sidecar meanwhile is not mistaken for a DJ edit.
    auto json = sidecarJson(1, 2, crateRefs({"crate_a"}));
    json["grid"] = QJsonObject{{"first_beat_s", 0.2}, {"bpm", 126.0}};
    ASSERT_TRUE(writeByRename(tracksPath(sidecarName(1)), QJsonDocument(json).toJson()));
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return track(1) && std::abs(track(1)->getBpm() - 126.0) < 1e-6; },
            kTimeoutMillis));
    EXPECT_FALSE(m_pService->store().trackState(djlibId(1)));

    // The database is back: no file changes, the retry alone stores it.
    *pFail = false;
    ASSERT_TRUE(QTest::qWaitFor(
            [this] {
                const auto state = m_pService->store().trackState(djlibId(1));
                return state && state->sidecarVersion == 2;
            },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    const auto state = m_pService->store().trackState(djlibId(1));
    EXPECT_TRUE(DjlibImporter::fingerprintsMatch(
            state->appliedBeats, DjlibImporter::beatsFingerprint(track(1)->getBeats())));
    EXPECT_FALSE(track(1)->isDirty());
    EXPECT_EQ(1, applied.count());
}

TEST_F(DjlibServiceTest, crateWritesThatFailAreRetried) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a", "crate_b"});
    writePackage(2);
    writeSidecar(2, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return hasDjlibBeats(1) && hasDjlibBeats(2); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));

    auto pCrateFailures = std::make_shared<int>(0);
    auto pMembershipFailures = std::make_shared<int>(0);
    djlib::setWriteFailureForTesting(
            [pCrateFailures, pMembershipFailures](const QString& what) {
                if (what == QStringLiteral("crates")) {
                    return (*pCrateFailures)++ < 2;
                }
                return what == QStringLiteral("membership") && (*pMembershipFailures)++ < 2;
            });
    // crates.json changes once; the crate and the membership only get
    // written by the retries.
    writeCrates({"crate_a", "crate_b"});
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return managedCrates(1) == QStringList({"crate_a", "crate_b"}); },
            kTimeoutMillis));
    EXPECT_GE(*pCrateFailures, 3);
    ASSERT_TRUE(waitIdle());

    // The same for a package that is gone.
    *pMembershipFailures = 0;
    const TrackId goneId = track(2)->getId();
    ASSERT_TRUE(QFile::remove(tracksPath(packageName(2))));
    ASSERT_TRUE(QFile::remove(tracksPath(sidecarName(2))));
    ASSERT_TRUE(QTest::qWaitFor(
            [this, goneId] { return managedCratesOf(goneId).isEmpty(); }, kTimeoutMillis));
    EXPECT_GE(*pMembershipFailures, 3);
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(-1, m_pService->store().trackState(djlibId(2))->sidecarVersion);
}

TEST_F(DjlibServiceTest, writeStillFailingWhenTheFolderChangesIsNotGivenUp) {
    writeCrates({"crate_a"});
    auto pFail = std::make_shared<bool>(true);
    auto pFailures = std::make_shared<int>(0);
    djlib::setWriteFailureForTesting([pFail, pFailures](const QString& what) {
        if (what == QStringLiteral("save") && *pFail) {
            ++*pFailures;
            return true;
        }
        return false;
    });
    startService();
    QSignalSpy applied(m_pService.get(), &DjlibService::sidecarApplied);
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    EXPECT_GE(*pFailures, 1);
    EXPECT_FALSE(m_pService->store().trackState(djlibId(1)));

    // Another folder is chosen while the track has the sidecar's values
    // but nothing says so yet: without the stored state they would be
    // taken for the DJ's own from then on.
    QTemporaryDir other;
    ASSERT_TRUE(other.isValid());
    m_pService->setLibraryPath(other.path());
    const int failuresBefore = *pFailures;
    ASSERT_TRUE(QTest::qWaitFor(
            [pFailures, failuresBefore] { return *pFailures > failuresBefore; },
            kTimeoutMillis));
    EXPECT_FALSE(m_pService->store().trackState(djlibId(1)));
    EXPECT_GT(m_pService->status().queued, 0);

    *pFail = false;
    ASSERT_TRUE(QTest::qWaitFor([&applied] { return applied.count() == 1; }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    const auto state = m_pService->store().trackState(djlibId(1));
    ASSERT_TRUE(state);
    EXPECT_TRUE(DjlibImporter::fingerprintsMatch(
            state->appliedBeats, DjlibImporter::beatsFingerprint(track(1)->getBeats())));
    EXPECT_FALSE(track(1)->isDirty());
}

TEST_F(DjlibServiceTest, trackThatCannotBeAddedIsRetried) {
    writeCrates({"crate_a"});
    auto pFailures = std::make_shared<int>(0);
    djlib::setWriteFailureForTesting([pFailures](const QString& what) {
        return what == QStringLiteral("add") && (*pFailures)++ < 2;
    });
    startService();
    // A package without sidecar, and one with: nothing changes on disk
    // afterwards, only the retry can add them.
    writePackage(1);
    writePackage(2);
    writeSidecar(2, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return track(1) && hasDjlibBeats(2); }, kTimeoutMillis));
    EXPECT_GE(*pFailures, 3);
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(2));
}

TEST_F(DjlibServiceTest, sidecarFileFieldMayDifferFromTheNameOnDisk) {
    writeCrates({"crate_a"});
    startService();
    // Renamed on disk; the sidecar's "file" still has the old name.
    const QString package = QStringLiteral("Artist - New Title [10000001].stem.mp4");
    const QString sidecar = QStringLiteral("Artist - New Title [10000001].djlib.json");
    ASSERT_TRUE(copyByRename(getTestDir().filePath(kAlacStem), tracksPath(package)));
    ASSERT_TRUE(writeByRename(tracksPath(sidecar),
            QJsonDocument(sidecarJson(1, 1, crateRefs({"crate_a"}))).toJson()));
    writePackage(2);
    writeSidecar(2, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(2); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    const auto pTrack = trackByPath(tracksPath(package));
    ASSERT_TRUE(pTrack);
    EXPECT_EQ(package, m_pService->store().trackState(djlibId(1))->file);

    // Another package goes: the sweep must not take track 1 for gone.
    ASSERT_TRUE(QFile::remove(tracksPath(packageName(2))));
    ASSERT_TRUE(QFile::remove(tracksPath(sidecarName(2))));
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return m_pService->store().trackState(djlibId(2))->sidecarVersion == -1; },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(pTrack->getId()));
    EXPECT_EQ(1, m_pService->store().trackState(djlibId(1))->sidecarVersion);
}

TEST_F(DjlibServiceTest, sweepRepairsRowWithAnOutdatedFileName) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    writePackage(2);
    writeSidecar(2, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return hasDjlibBeats(1) && hasDjlibBeats(2); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    // A row as an earlier version stored it: the sidecar's stale "file".
    auto state = m_pService->store().trackState(djlibId(1));
    ASSERT_TRUE(state);
    state->file = QStringLiteral("Artist - Old Title [10000001].stem.mp4");
    ASSERT_TRUE(m_pService->store().upsertTrackState(*state));

    ASSERT_TRUE(QFile::remove(tracksPath(packageName(2))));
    ASSERT_TRUE(QFile::remove(tracksPath(sidecarName(2))));
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return m_pService->store().trackState(djlibId(2))->sidecarVersion == -1; },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));
    EXPECT_EQ(1, m_pService->store().trackState(djlibId(1))->sidecarVersion);
    EXPECT_EQ(packageName(1), m_pService->store().trackState(djlibId(1))->file);
}

TEST_F(DjlibServiceTest, packageUnreadableAtFirstIsProbedAgainWhenReplaced) {
    writeCrates({"crate_a"});
    startService();
    // Not audio yet (e.g. a sync tool still filling the file).
    ASSERT_TRUE(writeByRename(tracksPath(packageName(1)), QByteArray(4096, 'x')));
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return track(1) != nullptr; }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    ASSERT_FALSE(hasDjlibBeats(1));

    // The real package arrives; the sidecar does not change.
    QTest::qWait(50);
    writePackage(1);
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(1, m_pService->store().trackState(djlibId(1))->sidecarVersion);
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));
}

TEST_F(DjlibServiceTest, brieflyEmptyCratesFileKeepsTheCrates) {
    m_emptyConfirmMillis = 1500;
    writeCrates({"crate_a", "crate_b"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    const CrateId crateId = m_pService->store().crateMapping("crate_a")->crateId;

    // A valid file without crates, replaced by the real list shortly after.
    writeCrates({});
    QTest::qWait(600);
    ASSERT_TRUE(waitIdle());
    ASSERT_TRUE(m_pService->store().crateMapping("crate_a"));
    writeCrates({"crate_a", "crate_b"});
    QTest::qWait(2000); // past the confirm delay
    ASSERT_TRUE(waitIdle());
    ASSERT_TRUE(m_pService->store().crateMapping("crate_a"));
    EXPECT_EQ(crateId, m_pService->store().crateMapping("crate_a")->crateId);
    EXPECT_TRUE(m_pService->store().crateMapping("crate_b"));
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));
}

TEST_F(DjlibServiceTest, cratesFileThatStaysEmptyRemovesTheCrates) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());

    writeCrates({});
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return !m_pService->store().crateMapping("crate_a"); },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    EXPECT_TRUE(managedCrates(1).isEmpty());
    EXPECT_TRUE(track(1));
}

TEST_F(DjlibServiceTest, metadataIsNeverExportedIntoTheLibraryFolder) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());

    // The same ordinary file inside and outside of the library folder.
    const QString mp3 = getTestDir().filePath(QStringLiteral("id3-test-data/artist.mp3"));
    QTemporaryDir otherDir;
    ASSERT_TRUE(otherDir.isValid());
    const QString inside = tracksPath(QStringLiteral("extra.mp3"));
    const QString outside = QDir(otherDir.path()).filePath(QStringLiteral("extra.mp3"));
    ASSERT_TRUE(QFile::copy(mp3, inside));
    ASSERT_TRUE(QFile::copy(mp3, outside));

    const auto bytesOf = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    // Both ways of asking for an export: explicitly and by the preference.
    const auto editAndSave = [this, &bytesOf](const QString& path, bool markForExport) {
        const QByteArray before = bytesOf(path);
        EXPECT_FALSE(before.isEmpty());
        {
            const TrackPointer pTrack = trackCollectionManager()->getOrAddTrack(
                    TrackRef::fromFilePath(path));
            EXPECT_TRUE(pTrack);
            if (!pTrack) {
                return false;
            }
            pTrack->setTitle(pTrack->getTitle() + QStringLiteral(" (edited)"));
            if (markForExport) {
                pTrack->markForMetadataExport();
            }
            trackCollectionManager()->saveTrack(pTrack);
        }
        return bytesOf(path) != before;
    };
    EXPECT_TRUE(editAndSave(outside, true)); // the export does work
    EXPECT_FALSE(editAndSave(inside, true));
    EXPECT_FALSE(editAndSave(tracksPath(packageName(1)), true));

    config()->setValue(mixxx::library::prefs::kSyncTrackMetadataConfigKey, 1);
    EXPECT_TRUE(editAndSave(outside, false));
    EXPECT_FALSE(editAndSave(inside, false));
    EXPECT_FALSE(editAndSave(tracksPath(packageName(1)), false));

    // A request that was skipped is withdrawn, not kept for later.
    {
        const TrackPointer pTrack = trackCollectionManager()->getOrAddTrack(
                TrackRef::fromFilePath(inside));
        ASSERT_TRUE(pTrack);
        pTrack->setComment(QStringLiteral("marked"));
        pTrack->markForMetadataExport();
        trackCollectionManager()->saveTrack(pTrack);
        EXPECT_FALSE(pTrack->isMarkedForMetadataExport());
    }

    // Another folder is chosen: the files of the old one are still the
    // agent's.
    QTemporaryDir newLibDir;
    ASSERT_TRUE(newLibDir.isValid());
    ASSERT_TRUE(QDir(newLibDir.path()).mkpath(QStringLiteral("tracks")));
    m_pService->setLibraryPath(newLibDir.path());
    ASSERT_TRUE(waitIdle());
    const QString insideNew =
            QDir(newLibDir.path()).filePath(QStringLiteral("tracks/extra.mp3"));
    ASSERT_TRUE(QFile::copy(mp3, insideNew));
    EXPECT_FALSE(editAndSave(insideNew, true));
    EXPECT_FALSE(editAndSave(inside, true));
    EXPECT_FALSE(editAndSave(tracksPath(packageName(1)), false));

    // Mixxx exits: the service goes first, the tracks still in use (loaded
    // in a deck, edited) are saved after it.
    const QByteArray before = bytesOf(inside);
    {
        const TrackPointer pTrack = trackCollectionManager()->getOrAddTrack(
                TrackRef::fromFilePath(inside));
        ASSERT_TRUE(pTrack);
        pTrack->setTitle(QStringLiteral("edited at exit"));
        pTrack->markForMetadataExport();
        stopService();
        trackCollectionManager()->saveTrack(pTrack);
    }
    EXPECT_EQ(before, bytesOf(inside));
    EXPECT_FALSE(editAndSave(inside, false));
    EXPECT_TRUE(editAndSave(outside, true)); // unlike everything else
}

TEST_F(DjlibServiceTest, libraryFoundAutomaticallyIsProtectedToo) {
    EXPECT_FALSE(djlib::hasProtectedRoots());
    writeAgentFile(m_libPath);
    createService();
    m_pService->start();
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return !m_pService->libraryPath().isEmpty(); }, kTimeoutMillis));
    EXPECT_TRUE(djlib::isProtectedFile(tracksPath(QStringLiteral("any.stem.mp4"))));
    EXPECT_FALSE(djlib::isProtectedFile(
            QDir(musicPath()).filePath(QStringLiteral("any.stem.mp4"))));
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
            [this, trackId] { return managedCratesOf(trackId).isEmpty(); },
            kTimeoutMillis));
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

TEST_F(DjlibServiceTest, preferencesPageSetsPathAndWarns) {
    startService();
    m_pService->setLibraryPath(QString());

    DlgPrefDjlib page(nullptr, config());
    auto* pLineEdit = page.findChild<QLineEdit*>(QStringLiteral("lineEditLibraryPath"));
    auto* pWarning = page.findChild<QLabel*>(QStringLiteral("labelWarning"));
    auto* pWatched = page.findChild<QLabel*>(QStringLiteral("labelWatched"));
    ASSERT_TRUE(pLineEdit && pWarning && pWatched);
    EXPECT_EQ(QString(), pLineEdit->text());
    EXPECT_TRUE(pWarning->isHidden());

    pLineEdit->setText(m_libDir.path());
    page.slotApply();
    EXPECT_EQ(m_libDir.path(), m_pService->libraryPath());
    EXPECT_EQ(m_libDir.path(), config()->getValueString(DjlibService::kLibraryPathKey));
    EXPECT_TRUE(pWatched->text().startsWith(QStringLiteral("Watching")))
            << pWatched->text().toStdString();

    // Picking <lib>/tracks by mistake selects the library folder.
    pLineEdit->setText(QDir(m_libDir.path()).filePath(QStringLiteral("tracks")));
    page.slotApply();
    EXPECT_EQ(QDir::cleanPath(m_libDir.path()), m_pService->libraryPath());

    KeyDetectionSettings(config()).setReanalyzeWhenSettingsChange(true);
    page.slotUpdate();
    EXPECT_FALSE(pWarning->isHidden());

    page.slotResetToDefaults();
    page.slotApply();
    EXPECT_EQ(QString(), m_pService->libraryPath());
}

namespace {

void addDjHotCue(const TrackPointer& pTrack) {
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            1, // hot cue 2
            mixxx::audio::FramePos(1000),
            mixxx::audio::kInvalidFramePos);
}

bool hasDjHotCue(const TrackPointer& pTrack) {
    const CuePointer pCue = pTrack ? pTrack->findHotcueByIndex(1) : CuePointer();
    return pCue && pCue->getPosition() == mixxx::audio::FramePos(1000);
}

} // namespace

TEST_F(DjlibServiceTest, removalWhileClosedDropsCrates) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    const TrackId trackId = track(1)->getId();
    stopService();

    // The agent moves both files to _removed while Mixxx is closed.
    ASSERT_TRUE(QDir(m_libPath).mkpath(QStringLiteral("_removed")));
    ASSERT_TRUE(QFile::rename(tracksPath(packageName(1)),
            QDir(m_libPath).filePath(QStringLiteral("_removed/") + packageName(1))));
    ASSERT_TRUE(QFile::rename(tracksPath(sidecarName(1)),
            QDir(m_libPath).filePath(QStringLiteral("_removed/") + sidecarName(1))));

    startServiceFromConfig();
    ASSERT_TRUE(QTest::qWaitFor(
            [this, trackId] { return managedCratesOf(trackId).isEmpty(); },
            kTimeoutMillis));
    EXPECT_TRUE(trackCollectionManager()->getTrackById(trackId));
    EXPECT_TRUE(m_pService->store().crateMapping("crate_a"));
}

TEST_F(DjlibServiceTest, renameWhileClosedRelocatesAndKeepsDjEdits) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    TrackId trackId;
    {
        const auto pTrack = track(1);
        trackId = pTrack->getId();
        addDjHotCue(pTrack);
        trackCollectionManager()->saveTrack(pTrack);
    }
    stopService();

    // Title fix while Mixxx is closed: new file name, same track_id, v2.
    const QString newName = QStringLiteral("Artist - Better Title [10000001].stem.mp4");
    ASSERT_TRUE(QFile::rename(tracksPath(packageName(1)), tracksPath(newName)));
    ASSERT_TRUE(QFile::remove(tracksPath(sidecarName(1))));
    QJsonObject json = sidecarJson(1, 2, crateRefs({"crate_a"}));
    json["file"] = newName;
    ASSERT_TRUE(writeByRename(
            tracksPath(QStringLiteral("Artist - Better Title [10000001].djlib.json")),
            QJsonDocument(json).toJson()));

    startServiceFromConfig();
    ASSERT_TRUE(QTest::qWaitFor(
            [this] {
                const auto state = m_pService->store().trackState(djlibId(1));
                return state && state->sidecarVersion == 2;
            },
            kTimeoutMillis));
    const auto pTrack = trackByPath(tracksPath(newName));
    ASSERT_TRUE(pTrack);
    EXPECT_EQ(trackId, pTrack->getId()); // same row: history, playlists stay
    EXPECT_TRUE(hasDjHotCue(pTrack));    // DJ edit kept
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(trackId));
    EXPECT_EQ(trackId, m_pService->store().trackState(djlibId(1))->trackId);
}

TEST_F(DjlibServiceTest, renameSeenPackageFirstKeepsTheLibraryEntry) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    TrackId trackId;
    {
        const auto pTrack = track(1);
        trackId = pTrack->getId();
        addDjHotCue(pTrack);
        trackCollectionManager()->saveTrack(pTrack);
    }
    ASSERT_TRUE(waitIdle());

    // Title fix while Mixxx runs, and a pass falls between the package and
    // its sidecar: the new name becomes a library track of its own first.
    const QString newName = QStringLiteral("Artist - Better Title [10000001].stem.mp4");
    ASSERT_TRUE(QFile::rename(tracksPath(packageName(1)), tracksPath(newName)));
    ASSERT_TRUE(QFile::remove(tracksPath(sidecarName(1))));
    ASSERT_TRUE(QTest::qWaitFor(
            [this, newName] { return trackByPath(tracksPath(newName)) != nullptr; },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    ASSERT_NE(trackId, trackByPath(tracksPath(newName))->getId());

    QJsonObject json = sidecarJson(1, 2, crateRefs({"crate_a"}));
    json["file"] = newName;
    ASSERT_TRUE(writeByRename(
            tracksPath(QStringLiteral("Artist - Better Title [10000001].djlib.json")),
            QJsonDocument(json).toJson()));
    ASSERT_TRUE(QTest::qWaitFor(
            [this] {
                const auto state = m_pService->store().trackState(djlibId(1));
                return state && state->sidecarVersion == 2;
            },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    const auto pTrack = trackByPath(tracksPath(newName));
    ASSERT_TRUE(pTrack);
    EXPECT_EQ(trackId, pTrack->getId()); // the old row, not the stand-in
    EXPECT_TRUE(hasDjHotCue(pTrack));
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(trackId));
    EXPECT_EQ(trackId, m_pService->store().trackState(djlibId(1))->trackId);
}

TEST_F(DjlibServiceTest, renameOntoAUsedLibraryEntryLetsItTakeOver) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    const TrackId trackId = track(1)->getId();
    ASSERT_TRUE(waitIdle());

    const QString newName = QStringLiteral("Artist - Better Title [10000001].stem.mp4");
    ASSERT_TRUE(QFile::rename(tracksPath(packageName(1)), tracksPath(newName)));
    ASSERT_TRUE(QFile::remove(tracksPath(sidecarName(1))));
    ASSERT_TRUE(QTest::qWaitFor(
            [this, newName] { return trackByPath(tracksPath(newName)) != nullptr; },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    TrackId newTrackId;
    {
        // The DJ already worked with the new entry: it must not be dropped.
        const auto pTrack = trackByPath(tracksPath(newName));
        newTrackId = pTrack->getId();
        addDjHotCue(pTrack);
        trackCollectionManager()->saveTrack(pTrack);
    }
    QJsonObject json = sidecarJson(1, 2, crateRefs({"crate_a"}));
    json["file"] = newName;
    ASSERT_TRUE(writeByRename(
            tracksPath(QStringLiteral("Artist - Better Title [10000001].djlib.json")),
            QJsonDocument(json).toJson()));
    ASSERT_TRUE(QTest::qWaitFor(
            [this, newTrackId] {
                const auto state = m_pService->store().trackState(djlibId(1));
                return state && state->trackId == newTrackId;
            },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    EXPECT_TRUE(hasDjHotCue(trackCollectionManager()->getTrackById(newTrackId)));
    EXPECT_TRUE(trackCollectionManager()->getTrackById(trackId));
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(newTrackId));
    EXPECT_TRUE(managedCratesOf(trackId).isEmpty());
}

TEST_F(DjlibServiceTest, renameOntoAnEntryNotAddedByTheServiceKeepsThatEntry) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    const TrackId trackId = track(1)->getId();
    ASSERT_TRUE(waitIdle());
    stopService();

    // Renamed while Mixxx was closed; the new file became a library track
    // without the service (a rescan, a drag and drop) and the DJ rated it.
    const QString newName = QStringLiteral("Artist - Better Title [10000001].stem.mp4");
    ASSERT_TRUE(QFile::rename(tracksPath(packageName(1)), tracksPath(newName)));
    ASSERT_TRUE(QFile::remove(tracksPath(sidecarName(1))));
    TrackId newTrackId;
    {
        const auto pTrack = trackCollectionManager()->getOrAddTrack(
                TrackRef::fromFilePath(tracksPath(newName)));
        ASSERT_TRUE(pTrack);
        newTrackId = pTrack->getId();
        ASSERT_NE(trackId, newTrackId);
        pTrack->setRating(4);
        pTrack->setComment(QStringLiteral("opener"));
        trackCollectionManager()->saveTrack(pTrack);
    }
    QJsonObject json = sidecarJson(1, 2, crateRefs({"crate_a"}));
    json["file"] = newName;
    ASSERT_TRUE(writeByRename(
            tracksPath(QStringLiteral("Artist - Better Title [10000001].djlib.json")),
            QJsonDocument(json).toJson()));

    startServiceFromConfig();
    ASSERT_TRUE(QTest::qWaitFor(
            [this, newTrackId] {
                const auto state = m_pService->store().trackState(djlibId(1));
                return state && state->trackId == newTrackId && state->sidecarVersion == 2;
            },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    // Not purged: it takes over, with what the DJ did to it.
    const auto pTrack = trackCollectionManager()->getTrackById(newTrackId);
    ASSERT_TRUE(pTrack);
    EXPECT_EQ(4, pTrack->getRating());
    EXPECT_EQ(QStringLiteral("opener"), pTrack->getComment());
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(newTrackId));
    EXPECT_TRUE(trackCollectionManager()->getTrackById(trackId));
}

TEST_F(DjlibServiceTest, libraryFolderMoveRelocatesTracks) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    TrackId trackId;
    {
        const auto pTrack = track(1);
        trackId = pTrack->getId();
        addDjHotCue(pTrack);
        trackCollectionManager()->saveTrack(pTrack);
    }
    stopService();

    // The DJ moves the whole library folder and points Mixxx at it.
    QTemporaryDir newLib;
    ASSERT_TRUE(newLib.isValid());
    for (const auto& dir : {QStringLiteral("tracks"), QStringLiteral("_djlib")}) {
        ASSERT_TRUE(QDir().rename(QDir(m_libPath).filePath(dir), QDir(newLib.path()).filePath(dir)));
    }
    m_libPath = newLib.path();

    startServiceFromConfig();
    ASSERT_TRUE(QTest::qWaitFor(
            [this, trackId] {
                const auto pTrack = track(1);
                return pTrack && pTrack->getId() == trackId;
            },
            kTimeoutMillis));
    EXPECT_TRUE(hasDjHotCue(track(1)));
    EXPECT_TRUE(hasDjlibBeats(1));
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(trackId));
    stopService(); // before newLib goes away
}

TEST_F(DjlibServiceTest, pathsAreComparedCleanedAndByThePlatformsCase) {
    EXPECT_TRUE(djlib::samePath(QStringLiteral("/a/lib"), QStringLiteral("/a/lib/")));
    EXPECT_TRUE(djlib::samePath(QStringLiteral("/a/lib"), QStringLiteral("/a//x/../lib/.")));
    EXPECT_FALSE(djlib::samePath(QStringLiteral("/a/lib"), QStringLiteral("/a/lib2")));
    EXPECT_FALSE(djlib::samePath(QStringLiteral("/a/lib"), QString()));
    EXPECT_TRUE(djlib::samePath(QString(), QString()));

    djlib::setPathCaseSensitivityForTesting(Qt::CaseSensitive);
    EXPECT_FALSE(djlib::samePath(QStringLiteral("C:/Users/Dj/Music/djlib"),
            QStringLiteral("c:/users/dj/music/DJLIB/")));
    // As on Windows and macOS.
    djlib::setPathCaseSensitivityForTesting(Qt::CaseInsensitive);
    EXPECT_TRUE(djlib::samePath(QStringLiteral("C:/Users/Dj/Music/djlib"),
            QStringLiteral("c:/users/dj/music/DJLIB/")));
    djlib::addProtectedRoot(QStringLiteral("C:/Users/Dj/Music/djlib"));
    EXPECT_TRUE(djlib::isProtectedFile(QStringLiteral("c:/users/dj/music/DJLIB/tracks/a.mp4")));
    EXPECT_FALSE(djlib::isProtectedFile(QStringLiteral("c:/users/dj/music/djlib2/a.mp4")));
}

TEST_F(DjlibServiceTest, folderChosenInAnotherSpellingIsNotRearmed) {
    djlib::setPathCaseSensitivityForTesting(Qt::CaseInsensitive);
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    const QString used = m_pService->libraryPath();
    int statusChanges = 0;
    QObject::connect(m_pService.get(),
            &DjlibService::statusChanged,
            m_pService.get(),
            [&statusChanges] { ++statusChanges; });
    // What Windows would hand out for the same folder.
    ASSERT_NE(used, used.toUpper());
    m_pService->setLibraryPath(used.toUpper() + QStringLiteral("/tracks/../"));
    EXPECT_EQ(used, m_pService->libraryPath());
    EXPECT_TRUE(m_pService->status().watching);
    EXPECT_EQ(0, statusChanges); // not armed again
    EXPECT_EQ(1, m_pService->status().trackFiles);
}

TEST_F(DjlibServiceTest, sameFolderReachedByAnotherPathKeepsTheLibraryEntries) {
#ifdef Q_OS_WIN
    // QFile::link makes a .lnk shortcut on Windows, not a second path to the folder.
    GTEST_SKIP() << "needs a symbolic link";
#endif
    // The library folder is known by two paths (a link here; on Windows
    // also another case, a junction, a mapped drive).
    QTemporaryDir linkDir;
    ASSERT_TRUE(linkDir.isValid());
    const QString linkPath = QDir(linkDir.path()).filePath(QStringLiteral("djlib"));
    ASSERT_TRUE(QFile::link(m_libPath, linkPath));
    const QString linkedPackage =
            QDir(linkPath).filePath(QStringLiteral("tracks/") + packageName(1));

    writeCrates({"crate_a"});
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    createService();
    m_pService->setLibraryPath(linkPath);
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                const auto state = m_pService->store().trackState(djlibId(1));
                return state && state->sidecarVersion == 1;
            },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    TrackId trackId;
    {
        const auto pTrack = trackByPath(linkedPackage);
        ASSERT_TRUE(pTrack);
        trackId = pTrack->getId();
        addDjHotCue(pTrack);
        trackCollectionManager()->saveTrack(pTrack);
    }
    const auto locationOf = [this](TrackId id) {
        return internalCollection()->getTrackDAO().getTrackLocation(id);
    };
    ASSERT_EQ(linkedPackage, locationOf(trackId));
    stopService();

    // Next start: the folder is named by its real path (the agent's
    // library.json instead of the old setting, say).
    startService();
    ASSERT_TRUE(waitIdle());
    ASSERT_TRUE(QTest::qWaitFor(
            [&] { return locationOf(trackId) == tracksPath(packageName(1)); },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    // The same entry, with the DJ's work; not a second one for the file.
    const auto state = m_pService->store().trackState(djlibId(1));
    ASSERT_TRUE(state);
    EXPECT_EQ(trackId, state->trackId);
    EXPECT_TRUE(hasDjHotCue(trackCollectionManager()->getTrackById(trackId)));
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(trackId));
    EXPECT_FALSE(trackIdAt(linkedPackage).isValid());
    EXPECT_EQ(trackId,
            trackIdAt(tracksPath(packageName(1))));

    // A newer sidecar goes to that entry too.
    writeSidecar(1, 2, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                const auto newer = m_pService->store().trackState(djlibId(1));
                return newer && newer->sidecarVersion == 2;
            },
            kTimeoutMillis));
    EXPECT_EQ(trackId, m_pService->store().trackState(djlibId(1))->trackId);
    stopService();
}

TEST_F(DjlibServiceTest, agentNamingTheSameFolderDifferentlyChangesNothing) {
#ifdef Q_OS_WIN
    // QFile::link makes a .lnk shortcut on Windows, not a second path to the folder.
    GTEST_SKIP() << "needs a symbolic link";
#endif
    m_safetyPassMillis = 300;
    QTemporaryDir linkDir;
    ASSERT_TRUE(linkDir.isValid());
    const QString linkPath = QDir(linkDir.path()).filePath(QStringLiteral("djlib"));
    ASSERT_TRUE(QFile::link(m_libPath, linkPath));

    writeCrates({"crate_a"});
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    writeAgentFile(linkPath);
    createService();
    m_pService->start();
    ASSERT_TRUE(QTest::qWaitFor(
            [&] {
                const auto state = m_pService->store().trackState(djlibId(1));
                return state && state->sidecarVersion == 1;
            },
            kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    ASSERT_EQ(linkPath, m_pService->libraryPath());
    const TrackId trackId = m_pService->store().trackState(djlibId(1))->trackId;

    // Same folder: by its real path, and spelled with detours.
    for (const QString& spelling : {m_libPath, QString(linkPath + QStringLiteral("//tracks/../"))}) {
        writeAgentFile(spelling);
        QTest::qWait(1000); // several lookups
        ASSERT_TRUE(waitIdle());
        EXPECT_EQ(linkPath, m_pService->libraryPath());
        EXPECT_TRUE(m_pService->status().watching);
        EXPECT_EQ(trackId, m_pService->store().trackState(djlibId(1))->trackId);
        EXPECT_FALSE(trackIdAt(tracksPath(packageName(1))).isValid());
    }
    stopService();
}

TEST_F(DjlibServiceTest, missingTracksDirKeepsCrates) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    const TrackId trackId = track(1)->getId();
    const QString tracksDir = QDir(m_libPath).filePath(QStringLiteral("tracks"));
    const QString awayDir = QDir(m_libPath).filePath(QStringLiteral("tracks-away"));

    // Drive gone while running.
    ASSERT_TRUE(QDir().rename(tracksDir, awayDir));
    m_pService->requestPass();
    QTest::qWait(300);
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(trackId));
    EXPECT_EQ(1, m_pService->store().trackState(djlibId(1))->sidecarVersion);

    // ... and at start.
    stopService();
    startServiceFromConfig();
    QTest::qWait(300);
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(trackId));

    // Back again: nothing to do, still a member.
    ASSERT_TRUE(QDir().rename(awayDir, tracksDir));
    m_pService->requestPass();
    QTest::qWait(300);
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(trackId));
    EXPECT_EQ(1, m_pService->store().trackState(djlibId(1))->sidecarVersion);
}

TEST_F(DjlibServiceTest, unreadableSidecarIsRetried) {
    writeCrates({"crate_a"});
    auto pFailures = std::make_shared<std::atomic<int>>(0);
    djlib::setReadFailureForTesting([pFailures](const QString& path) {
        // The first two reads fail like a Windows sharing violation.
        return path.endsWith(sidecarName(1)) && pFailures->fetch_add(1) < 2;
    });
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    // No further change to the file: only the retry can import it.
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    EXPECT_GE(pFailures->load(), 3);
    EXPECT_EQ(QStringList{"crate_a"}, managedCrates(1));
}

TEST_F(DjlibServiceTest, passStuckOnTheFileSystemDoesNotKeepMixxxFromClosing) {
    m_exitWaitMillis = 300;
    struct Gate {
        QSemaphore entered;
        QSemaphore release;
        std::atomic<bool> left{false};
    };
    auto pGate = std::make_shared<Gate>();
    djlib::setReadFailureForTesting([pGate](const QString& path) {
        if (path.endsWith(sidecarName(1))) {
            // A read on a share that went away: it does not come back.
            pGate->entered.release();
            pGate->release.acquire();
            pGate->left.store(true);
        }
        return false;
    });
    writeCrates({"crate_a"});
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    writePackage(2);
    writeSidecar(2, 1, {"crate_a"});
    startService();
    ASSERT_TRUE(pGate->entered.tryAcquire(1, kTimeoutMillis));

    QElapsedTimer timer;
    timer.start();
    stopService();
    EXPECT_LT(timer.elapsed(), 2000);

    // The pass left behind ends by itself once the read returns; nothing
    // it touches went away with the service.
    pGate->release.release(100);
    ASSERT_TRUE(QTest::qWaitFor([&pGate] { return pGate->left.load(); }, kTimeoutMillis));
    QTest::qWait(300);
    EXPECT_FALSE(track(1));
}

TEST_F(DjlibServiceTest, unreadableCratesJsonIsRetried) {
    writeCrates({"crate_a"});
    auto pFailures = std::make_shared<std::atomic<int>>(0);
    djlib::setReadFailureForTesting([pFailures](const QString& path) {
        return path.endsWith(QStringLiteral("crates.json")) && pFailures->fetch_add(1) < 1;
    });
    startService();
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return m_pService->store().crateMapping("crate_a").has_value(); },
            kTimeoutMillis));
    EXPECT_GE(pFailures->load(), 2);
}

TEST_F(DjlibServiceTest, libraryPathLogic) {
    // <music>/djlib is the default only if it exists.
    QTemporaryDir music;
    ASSERT_TRUE(music.isValid());
    EXPECT_EQ(QString(), DjlibService::defaultLibraryPath(music.path()));
    ASSERT_TRUE(QDir(music.path()).mkpath(QStringLiteral("djlib")));
    EXPECT_EQ(QDir(music.path()).filePath(QStringLiteral("djlib")),
            DjlibService::defaultLibraryPath(music.path()));
    EXPECT_EQ(QString(), DjlibService::defaultLibraryPath(QString()));

    // tracks/ next to _djlib -> the parent; other folders stay.
    const QString lib = QDir::cleanPath(m_libDir.path());
    EXPECT_EQ(lib, DjlibService::normalizeLibraryPath(lib + QStringLiteral("/tracks")));
    EXPECT_EQ(lib, DjlibService::normalizeLibraryPath(lib + QStringLiteral("/tracks/")));
    EXPECT_EQ(lib, DjlibService::normalizeLibraryPath(QStringLiteral("  ") + lib + "/ "));
    EXPECT_EQ(lib + QStringLiteral("/_djlib"),
            DjlibService::normalizeLibraryPath(lib + QStringLiteral("/_djlib")));
    const QString otherTracks = QDir::cleanPath(music.path()) + QStringLiteral("/tracks");
    ASSERT_TRUE(QDir().mkpath(otherTracks)); // no _djlib sibling
    EXPECT_EQ(otherTracks, DjlibService::normalizeLibraryPath(otherTracks));
    EXPECT_EQ(QString(), DjlibService::normalizeLibraryPath(QStringLiteral("  ")));
}

TEST_F(DjlibServiceTest, withoutSettingAndAgentItStaysOff) {
    // Absent and empty setting are the same: automatic, nothing found.
    createService();
    m_pService->start();
    ASSERT_TRUE(waitIdle());
    EXPECT_TRUE(m_pService->isAutomaticPath());
    EXPECT_EQ(QString(), m_pService->libraryPath());
    EXPECT_FALSE(m_pService->status().watching);
    stopService();

    config()->setValue(DjlibService::kLibraryPathKey, QString());
    createService();
    m_pService->start();
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QString(), m_pService->libraryPath());
    EXPECT_FALSE(m_pService->status().watching);
}

TEST_F(DjlibServiceTest, agentLibraryFileIsParsedStrictly) {
    using State = djlib::AgentLibrary::State;
    const QString path = agentFilePath();
    EXPECT_EQ(State::Absent, djlib::readAgentLibraryFile(path).state);

    writeAgentFile(m_libPath + QStringLiteral("/"));
    const auto found = djlib::readAgentLibraryFile(path);
    EXPECT_EQ(State::Found, found.state);
    EXPECT_EQ(QDir::cleanPath(m_libPath), found.libraryDir);

    const QList<QByteArray> invalid{
            QByteArray(),
            QByteArrayLiteral("{\"schema\":1,\"library_dir\":"),
            QByteArrayLiteral("[]"),
            QByteArrayLiteral("{\"schema\":2,\"library_dir\":\"/music/djlib\"}"),
            QByteArrayLiteral("{\"schema\":\"1\",\"library_dir\":\"/music/djlib\"}"),
            QByteArrayLiteral("{\"library_dir\":\"/music/djlib\"}"),
            QByteArrayLiteral("{\"schema\":1}"),
            QByteArrayLiteral("{\"schema\":1,\"library_dir\":42}"),
            QByteArrayLiteral("{\"schema\":1,\"library_dir\":\"  \"}"),
            QByteArrayLiteral("{\"schema\":1,\"library_dir\":\"Music/djlib\"}"),
            QByteArrayLiteral("{\"schema\":1,\"library_dir\":\"/music/djlib\",\"x\":\"") +
                    QByteArray(70000, 'x') + QByteArrayLiteral("\"}"),
    };
    for (const auto& json : invalid) {
        ASSERT_TRUE(writeByRename(path, json));
        EXPECT_EQ(State::Invalid, djlib::readAgentLibraryFile(path).state)
                << json.left(80).toStdString();
    }
}

TEST_F(DjlibServiceTest, agentLibraryIsUsedWhenNoFolderIsChosen) {
    writeAgentFile(m_libPath);
    writeCrates({"crate_a"});
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    createService();
    m_pService->start();
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    EXPECT_TRUE(m_pService->isAutomaticPath());
    EXPECT_TRUE(m_pService->status().automatic);
    EXPECT_EQ(QDir::cleanPath(m_libPath), m_pService->libraryPath());
    EXPECT_TRUE(m_pService->status().watching);
    // Found, not chosen: the setting stays empty, so a later move is followed.
    EXPECT_EQ(QString(), config()->getValueString(DjlibService::kLibraryPathKey));
}

TEST_F(DjlibServiceTest, agentLibraryAppearingOrMovingLaterIsPickedUp) {
    m_safetyPassMillis = 300;
    writeCrates({"crate_a"});
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    createService();
    m_pService->start();
    ASSERT_TRUE(waitIdle());
    ASSERT_EQ(QString(), m_pService->libraryPath());

    // The agent is installed and run while Mixxx is open.
    writeAgentFile(m_libPath);
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    EXPECT_EQ(QDir::cleanPath(m_libPath), m_pService->libraryPath());

    // A file that can't be used (half-written, other schema) changes nothing.
    ASSERT_TRUE(writeByRename(agentFilePath(), QByteArrayLiteral("{\"schema\":1,")));
    QTest::qWait(800);
    EXPECT_EQ(QDir::cleanPath(m_libPath), m_pService->libraryPath());
    EXPECT_TRUE(m_pService->status().watching);

    // The DJ moves the library in the agent's settings.
    QTemporaryDir newLib;
    ASSERT_TRUE(newLib.isValid());
    ASSERT_TRUE(QDir(newLib.path()).mkpath(QStringLiteral("tracks")));
    ASSERT_TRUE(QDir(newLib.path()).mkpath(QStringLiteral("_djlib")));
    writeAgentFile(newLib.path());
    ASSERT_TRUE(QTest::qWaitFor(
            [this, &newLib] {
                return m_pService->libraryPath() == QDir::cleanPath(newLib.path()) &&
                        m_pService->status().watching;
            },
            kTimeoutMillis));
    EXPECT_EQ(QString(), config()->getValueString(DjlibService::kLibraryPathKey));
    stopService(); // before newLib goes
}

TEST_F(DjlibServiceTest, chosenFolderWinsOverTheAgentLibrary) {
    m_safetyPassMillis = 300;
    QTemporaryDir agentLib;
    ASSERT_TRUE(agentLib.isValid());
    ASSERT_TRUE(QDir(agentLib.path()).mkpath(QStringLiteral("tracks")));
    writeAgentFile(agentLib.path());
    startServiceFromConfig(); // m_libPath is in the settings
    QTest::qWait(800);        // several lookups would have run
    ASSERT_TRUE(waitIdle());
    EXPECT_FALSE(m_pService->isAutomaticPath());
    EXPECT_EQ(m_libPath, m_pService->libraryPath());
    EXPECT_EQ(m_libPath, config()->getValueString(DjlibService::kLibraryPathKey));

    // Cleared in the preferences: automatic again, the agent's folder.
    m_pService->setLibraryPath(QString());
    ASSERT_TRUE(QTest::qWaitFor(
            [this, &agentLib] {
                return m_pService->libraryPath() == QDir::cleanPath(agentLib.path());
            },
            kTimeoutMillis));
    EXPECT_TRUE(m_pService->isAutomaticPath());

    // And chosen again while a lookup may be running.
    m_pService->setLibraryPath(m_libPath);
    QTest::qWait(800);
    EXPECT_EQ(m_libPath, m_pService->libraryPath());
    stopService();
}

TEST_F(DjlibServiceTest, musicFolderDefaultIsUsedWithoutAgentFile) {
    m_safetyPassMillis = 300;
    createService();
    m_pService->start();
    ASSERT_TRUE(waitIdle());
    ASSERT_EQ(QString(), m_pService->libraryPath());

    // <Music>/djlib appears later (first sync of an agent without the file).
    const QString defaultLib = QDir(musicPath()).filePath(QStringLiteral("djlib"));
    ASSERT_TRUE(QDir().mkpath(defaultLib + QStringLiteral("/tracks")));
    ASSERT_TRUE(QTest::qWaitFor(
            [this, &defaultLib] { return m_pService->libraryPath() == defaultLib; },
            kTimeoutMillis));
    EXPECT_EQ(QString(), config()->getValueString(DjlibService::kLibraryPathKey));

    // The agent's file, once there, is the better source.
    writeAgentFile(m_libPath);
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return m_pService->libraryPath() == QDir::cleanPath(m_libPath); },
            kTimeoutMillis));
}

TEST_F(DjlibServiceTest, preferencesPageShowsAutomaticFolder) {
    createService();
    m_pService->start();
    ASSERT_TRUE(waitIdle());
    DlgPrefDjlib page(nullptr, config());
    auto* pLineEdit = page.findChild<QLineEdit*>(QStringLiteral("lineEditLibraryPath"));
    auto* pWatched = page.findChild<QLabel*>(QStringLiteral("labelWatched"));
    ASSERT_TRUE(pLineEdit && pWatched);
    EXPECT_TRUE(pWatched->text().startsWith(QStringLiteral("No djlib library found yet")))
            << pWatched->text().toStdString();

    writeAgentFile(m_libPath);
    m_pService->setLibraryPath(QString()); // as Apply with an empty field: looks again
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return m_pService->status().watching; }, kTimeoutMillis));
    ASSERT_TRUE(waitIdle());
    page.slotUpdate();
    EXPECT_EQ(QString(), pLineEdit->text()); // nothing chosen
    EXPECT_TRUE(pWatched->text().contains(QStringLiteral("found automatically")))
            << pWatched->text().toStdString();
    EXPECT_TRUE(pWatched->text().contains(QDir::toNativeSeparators(m_libPath)))
            << pWatched->text().toStdString();
}

TEST_F(DjlibServiceTest, statusShowsMissingTracksFolder) {
    QTemporaryDir other;
    ASSERT_TRUE(other.isValid());
    createService();
    // The status is what the last pass saw.
    m_pService->setLibraryPath(other.path());
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringLiteral("no-tracks-folder"), m_pService->status().problem);
    EXPECT_FALSE(m_pService->status().watching);
    m_pService->setLibraryPath(other.path() + QStringLiteral("/nope"));
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringLiteral("no-library-folder"), m_pService->status().problem);
    m_pService->setLibraryPath(m_libDir.path());
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QString(), m_pService->status().problem);
    EXPECT_TRUE(m_pService->status().watching);

    // tracks/ created later is picked up and watched.
    m_pService->setLibraryPath(other.path());
    ASSERT_TRUE(waitIdle());
    ASSERT_TRUE(QDir(other.path()).mkpath(QStringLiteral("tracks")));
    m_pService->requestPass();
    ASSERT_TRUE(QTest::qWaitFor(
            [this] { return m_pService->status().watching; }, kTimeoutMillis));
    EXPECT_EQ(QString(), m_pService->status().problem);
}

TEST_F(DjlibServiceTest, brieflyEmptyTracksDirKeepsCrates) {
    m_emptyConfirmMillis = 3000;
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 1, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));
    const TrackId trackId = track(1)->getId();
    EXPECT_EQ(1, m_pService->status().trackFiles);

    // The folder is emptied (e.g. recreated by the agent) and refilled.
    QTemporaryDir away;
    ASSERT_TRUE(QFile::rename(tracksPath(packageName(1)), away.filePath(packageName(1))));
    ASSERT_TRUE(QFile::rename(tracksPath(sidecarName(1)), away.filePath(sidecarName(1))));
    m_pService->requestPass();
    QTest::qWait(300);
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(trackId));
    ASSERT_TRUE(QFile::rename(away.filePath(packageName(1)), tracksPath(packageName(1))));
    ASSERT_TRUE(QFile::rename(away.filePath(sidecarName(1)), tracksPath(sidecarName(1))));
    m_pService->requestPass();
    QTest::qWait(3500); // past the confirmation time
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOf(trackId));
    EXPECT_EQ(1, m_pService->store().trackState(djlibId(1))->sidecarVersion);
}

TEST_F(DjlibServiceTest, staleSidecarWithoutPackageIsIgnored) {
    writeCrates({"crate_a"});
    startService();
    writePackage(1);
    writeSidecar(1, 2, {"crate_a"});
    ASSERT_TRUE(QTest::qWaitFor([this] { return hasDjlibBeats(1); }, kTimeoutMillis));

    // An old-name sidecar (v1) left behind without its package.
    QJsonObject json = sidecarJson(1, 1, crateRefs({"crate_a"}));
    json["file"] = QStringLiteral("Old Name [10000001].stem.mp4");
    ASSERT_TRUE(writeByRename(tracksPath(QStringLiteral("Old Name [10000001].djlib.json")),
            QJsonDocument(json).toJson()));
    m_pService->requestPass();
    QTest::qWait(300);
    ASSERT_TRUE(waitIdle());
    EXPECT_EQ(0, m_pService->status().waitingForPackage);
    EXPECT_FALSE(m_pService->store().trackState(djlibId(1))->pendingJson);
}
