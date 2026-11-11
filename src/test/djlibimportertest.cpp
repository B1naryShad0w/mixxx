#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "analyzer/analyzerkey.h"
#include "analyzer/analyzertrack.h"
#include "library/djlib/djlibcratesync.h"
#include "library/djlib/djlibimporter.h"
#include "library/djlib/djlibstore.h"
#include "preferences/keydetectionsettings.h"
#include "proto/keys.pb.h"
#include "sources/soundsourceproxy.h"
#include "test/librarytest.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/keyfactory.h"
#include "track/track.h"
#include "util/math.h"

using Field = DjlibImporter::Field;
using mixxx::audio::FramePos;
namespace key = mixxx::track::io::key;

namespace {

const QString kStemFile = QStringLiteral("stems/stem01/sin_ALAC_24bit.stem.mp4");
const QString kTrackFile = QStringLiteral("Artist - Title [12345678].stem.mp4");
const QString kDjlibId = QStringLiteral("trk_1");

/// Builds a valid schema-1 sidecar; tests modify the JSON object before parsing.
QJsonObject sidecarJson(qint64 version) {
    return QJsonObject{
            {"schema", 1},
            {"track_id", kDjlibId},
            {"tidal_id", "12345678"},
            {"isrc", QJsonValue()},
            {"sidecar_version", version},
            {"package_version", 1},
            {"file", kTrackFile},
            {"sha256", QString(64, QChar('a'))},
            {"crates",
                    QJsonArray{QJsonObject{{"id", "crate_a"}, {"name", "House"}},
                            QJsonObject{{"id", "crate_other"}, {"name", "Not mine"}}}},
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

DjlibSidecar parse(const QJsonObject& json) {
    QString error;
    auto sidecar = DjlibSidecar::parse(QJsonDocument(json).toJson(), &error);
    EXPECT_TRUE(sidecar.has_value()) << error.toStdString();
    return sidecar.value_or(DjlibSidecar());
}

} // namespace

class DjlibImporterTest : public LibraryTest {
  protected:
    DjlibImporterTest()
            : m_store(internalCollection()->database()),
              m_sync(internalCollection(), &m_store),
              m_importer(&m_sync) {
        EXPECT_TRUE(m_store.initialize());
        EXPECT_TRUE(m_tempDir.isValid());
        const QString path = m_tempDir.filePath(kTrackFile);
        EXPECT_TRUE(QFile::copy(getTestDir().filePath(kStemFile), path));
        m_pTrack = getOrAddTrackByLocation(path);
        EXPECT_TRUE(m_pTrack);
        if (m_pTrack && !m_pTrack->getSampleRate().isValid()) {
            // STEM files may lack audio properties from TagLib: probe like
            // the service does (research §2).
            SoundSourceProxy(m_pTrack).openAudioSource();
        }
        EXPECT_TRUE(m_pTrack && m_pTrack->getSampleRate().isValid());
        EXPECT_TRUE(m_sync.reconcile({{"crate_a", "House"}, {"crate_b", "Techno"}}));
    }

    DjlibImporter::ApplyResult apply(const QJsonObject& json,
            bool trackLoaded = false,
            const DjlibImporter::Baseline& baseline = DjlibImporter::Baseline()) {
        return m_importer.apply(m_pTrack,
                parse(json),
                m_store,
                trackLoaded,
                QSet<QString>{"crate_a", "crate_b"},
                baseline);
    }

    double sampleRate() const {
        return m_pTrack->getSampleRate().toDouble();
    }

    FramePos frames(double seconds) const {
        return FramePos(seconds * sampleRate());
    }

    QStringList managedCratesOfTrack() const {
        QStringList result;
        auto select = internalCollection()->crates().selectTrackCratesSorted(m_pTrack->getId());
        while (select.next()) {
            const auto mapping = m_store.crateMappingByCrateId(select.crateId());
            if (mapping) {
                result << mapping->djlibId;
            }
        }
        result.sort();
        return result;
    }

    QList<CuePointer> hotCues() const {
        QList<CuePointer> result;
        for (const auto& pCue : m_pTrack->getCuePoints()) {
            if (pCue->getHotCue() != Cue::kNoHotCue) {
                result << pCue;
            }
        }
        return result;
    }

    void expectFramesNear(FramePos expected, FramePos actual) {
        ASSERT_TRUE(actual.isValid());
        EXPECT_NEAR(expected.value(), actual.value(), 0.5);
    }

    QTemporaryDir m_tempDir;
    DjlibStore m_store;
    DjlibCrateSync m_sync;
    DjlibImporter m_importer;
    TrackPointer m_pTrack;
};

TEST_F(DjlibImporterTest, constantGrid) {
    const auto result = apply(sidecarJson(3));
    EXPECT_FALSE(result.outdated);
    EXPECT_EQ(Field::Applied, result.beats);

    const auto pBeats = m_pTrack->getBeats();
    ASSERT_TRUE(pBeats);
    EXPECT_EQ(QStringLiteral("BeatGrid-2.0"), pBeats->getVersion());
    EXPECT_EQ(QStringLiteral("djlib/1"), pBeats->getSubVersion());
    EXPECT_TRUE(pBeats->hasConstantTempo());
    expectFramesNear(frames(0.1234), pBeats->firstBeat());
    EXPECT_NEAR(124.0, m_pTrack->getBpm(), 1e-6);
    EXPECT_TRUE(m_pTrack->isBpmLocked());

    const auto state = m_store.trackState(kDjlibId);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(3, state->sidecarVersion);
    EXPECT_EQ(m_pTrack->getId(), state->trackId);
    EXPECT_EQ(kTrackFile, state->file);
    EXPECT_FALSE(state->appliedBeats.isEmpty());
}

TEST_F(DjlibImporterTest, variableBeatMap) {
    auto json = sidecarJson(3);
    json["tempo"] = "variable";
    json["grid"] = QJsonValue();
    json["beats_s"] = QJsonArray{0.5, 1.0, 1.49, 2.0, 2.52, 3.0, 3.5};
    const auto result = apply(json);
    EXPECT_EQ(Field::Applied, result.beats);

    const auto pBeats = m_pTrack->getBeats();
    ASSERT_TRUE(pBeats);
    EXPECT_EQ(QStringLiteral("BeatMap-1.0"), pBeats->getVersion());
    EXPECT_EQ(QStringLiteral("djlib/1"), pBeats->getSubVersion());
    EXPECT_FALSE(pBeats->hasConstantTempo());
    // Beats::firstBeat() extrapolates before the first marker; check the map.
    ASSERT_FALSE(pBeats->getMarkers().empty());
    expectFramesNear(frames(0.5), pBeats->getMarkers().front().position());
    expectFramesNear(frames(0.5), pBeats->findNextBeat(frames(0.4)));
    expectFramesNear(frames(1.49), pBeats->findNthBeat(frames(0.5), 3));
    expectFramesNear(frames(2.52), pBeats->findNthBeat(frames(0.5), 5));
    EXPECT_TRUE(m_pTrack->isBpmLocked());
}

TEST_F(DjlibImporterTest, cues) {
    const auto result = apply(sidecarJson(3));
    EXPECT_EQ(Field::Applied, result.cues);

    expectFramesNear(frames(0.5795), m_pTrack->getMainCuePosition());
    const auto pMain = m_pTrack->findCueByType(mixxx::CueType::MainCue);
    ASSERT_TRUE(pMain);
    expectFramesNear(frames(0.5795), pMain->getPosition());

    const auto hot = hotCues();
    ASSERT_EQ(1, hot.size());
    const auto pHot = m_pTrack->findHotcueByIndex(0);
    ASSERT_TRUE(pHot);
    EXPECT_EQ(mixxx::CueType::HotCue, pHot->getType());
    expectFramesNear(frames(0.5795), pHot->getPosition());
    EXPECT_EQ(QStringLiteral("1st downbeat"), pHot->getLabel());
}

TEST_F(DjlibImporterTest, key) {
    const auto result = apply(sidecarJson(3));
    EXPECT_EQ(Field::Applied, result.key);
    EXPECT_EQ(key::A_MINOR, m_pTrack->getKey());
    EXPECT_EQ(DjlibImporter::kKeySubVersion, m_pTrack->getKeys().getSubVersion());
    {
        const QByteArray bytes = m_pTrack->getKeys().toByteArray();
        key::KeyMap keyMap;
        ASSERT_TRUE(keyMap.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())));
        EXPECT_EQ(key::FILE_METADATA, keyMap.source());
    }
    EXPECT_EQ(std::optional<int>(key::A_MINOR), m_store.trackState(kDjlibId)->appliedKey);
}

TEST_F(DjlibImporterTest, replayGain) {
    const auto result = apply(sidecarJson(3));
    EXPECT_TRUE(result.replayGainApplied);
    const auto rg = m_pTrack->getReplayGain();
    EXPECT_NEAR(db2ratio(-9.0), rg.getRatio(), 1e-9);
    EXPECT_NEAR(0.944, rg.getPeak(), 1e-6);

    // null leaves it alone
    auto json = sidecarJson(4);
    json["replaygain"] = QJsonValue();
    EXPECT_FALSE(apply(json).replayGainApplied);
    EXPECT_NEAR(db2ratio(-9.0), m_pTrack->getReplayGain().getRatio(), 1e-9);
}

TEST_F(DjlibImporterTest, cratesAreSidecarIntersectAllowed) {
    const auto result = apply(sidecarJson(3));
    EXPECT_TRUE(result.cratesApplied);
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOfTrack());

    auto json = sidecarJson(4);
    json["crates"] = QJsonArray{QJsonObject{{"id", "crate_b"}, {"name", "Techno"}}};
    EXPECT_TRUE(apply(json).cratesApplied);
    EXPECT_EQ(QStringList{"crate_b"}, managedCratesOfTrack());

    // crate_b managed but no longer in crates.json -> not a member
    EXPECT_TRUE(m_importer
                    .apply(m_pTrack,
                            parse(sidecarJson(5)),
                            m_store,
                            false,
                            QSet<QString>{"crate_b"})
                    .cratesApplied);
    EXPECT_TRUE(managedCratesOfTrack().isEmpty());
}

TEST_F(DjlibImporterTest, outdatedSidecarIsIgnored) {
    apply(sidecarJson(3));
    auto json = sidecarJson(3);
    json["key"] = "C";
    const auto same = apply(json);
    EXPECT_TRUE(same.outdated);
    EXPECT_EQ(key::A_MINOR, m_pTrack->getKey());
    json["sidecar_version"] = 2;
    EXPECT_TRUE(apply(json).outdated);
    EXPECT_EQ(key::A_MINOR, m_pTrack->getKey());
}

TEST_F(DjlibImporterTest, newerSidecarReplacesUneditedFields) {
    apply(sidecarJson(3));
    auto json = sidecarJson(4);
    json["grid"] = QJsonObject{{"first_beat_s", 0.2}, {"bpm", 126.0}};
    json["key"] = "C";
    json["cues"] = QJsonArray{QJsonObject{{"type", "main"}, {"pos_s", 1.0}},
            QJsonObject{{"type", "hotcue"}, {"number", 1}, {"pos_s", 1.0}, {"label", "1st downbeat"}}};
    const auto result = apply(json);
    EXPECT_EQ(Field::Applied, result.beats);
    EXPECT_EQ(Field::Applied, result.cues);
    EXPECT_EQ(Field::Applied, result.key);
    expectFramesNear(frames(0.2), m_pTrack->getBeats()->firstBeat());
    EXPECT_NEAR(126.0, m_pTrack->getBpm(), 1e-6);
    EXPECT_TRUE(m_pTrack->isBpmLocked());
    expectFramesNear(frames(1.0), m_pTrack->getMainCuePosition());
    ASSERT_EQ(1, hotCues().size());
    expectFramesNear(frames(1.0), m_pTrack->findHotcueByIndex(0)->getPosition());
    EXPECT_EQ(key::C_MAJOR, m_pTrack->getKey());
    EXPECT_EQ(4, m_store.trackState(kDjlibId)->sidecarVersion);
}

TEST_F(DjlibImporterTest, djEditsArePreservedPerField) {
    apply(sidecarJson(3));

    // The DJ moves the grid, adds hot cue 2 and changes the key.
    m_pTrack->setBpmLocked(false);
    const auto moved = m_pTrack->getBeats()->tryTranslate(100);
    ASSERT_TRUE(moved.has_value());
    ASSERT_TRUE(m_pTrack->trySetBeats(*moved));
    m_pTrack->setBpmLocked(true);
    m_pTrack->createAndAddCue(mixxx::CueType::HotCue, 1, frames(5.0), mixxx::audio::kInvalidFramePos);
    m_pTrack->setKey(key::G_MAJOR, key::USER);
    const QString djBeats = DjlibImporter::beatsFingerprint(m_pTrack->getBeats());

    auto json = sidecarJson(4);
    json["grid"] = QJsonObject{{"first_beat_s", 0.2}, {"bpm", 126.0}};
    json["key"] = "C";
    json["cues"] = QJsonArray{QJsonObject{{"type", "main"}, {"pos_s", 1.0}},
            QJsonObject{{"type", "hotcue"}, {"number", 1}, {"pos_s", 1.0}, {"label", "1st downbeat"}}};
    json["crates"] = QJsonArray{QJsonObject{{"id", "crate_b"}, {"name", "Techno"}}};
    json["replaygain"] = QJsonObject{{"gain_db", -6.0}, {"peak", 0.5}};
    const auto result = apply(json);

    EXPECT_EQ(Field::KeptDjEdit, result.beats);
    EXPECT_EQ(Field::KeptDjEdit, result.cues);
    EXPECT_EQ(Field::KeptDjEdit, result.key);
    EXPECT_TRUE(DjlibImporter::fingerprintsMatch(
            djBeats, DjlibImporter::beatsFingerprint(m_pTrack->getBeats())));
    // (tryTranslate() snaps the anchor to a whole frame)
    EXPECT_NEAR((frames(0.1234) + 100).value(), m_pTrack->getBeats()->firstBeat().value(), 1.0);
    expectFramesNear(frames(0.5795), m_pTrack->getMainCuePosition());
    EXPECT_EQ(2, hotCues().size());
    expectFramesNear(frames(0.5795), m_pTrack->findHotcueByIndex(0)->getPosition());
    EXPECT_EQ(key::G_MAJOR, m_pTrack->getKey());
    // Crates and ReplayGain always update.
    EXPECT_EQ(QStringList{"crate_b"}, managedCratesOfTrack());
    EXPECT_NEAR(db2ratio(-6.0), m_pTrack->getReplayGain().getRatio(), 1e-9);
    EXPECT_EQ(4, m_store.trackState(kDjlibId)->sidecarVersion);

    // The DJ's edits keep winning on later sidecars too.
    json["sidecar_version"] = 5;
    const auto again = apply(json);
    EXPECT_EQ(Field::KeptDjEdit, again.beats);
    EXPECT_EQ(Field::KeptDjEdit, again.cues);
    EXPECT_EQ(Field::KeptDjEdit, again.key);
    EXPECT_EQ(key::G_MAJOR, m_pTrack->getKey());
}

TEST_F(DjlibImporterTest, djEditOfOneFieldDoesNotBlockOthers) {
    apply(sidecarJson(3));
    m_pTrack->findHotcueByIndex(0)->setLabel(QStringLiteral("drop"));

    auto json = sidecarJson(4);
    json["grid"] = QJsonObject{{"first_beat_s", 0.2}, {"bpm", 126.0}};
    json["key"] = "C";
    json["cues"] = QJsonArray{QJsonObject{{"type", "main"}, {"pos_s", 1.0}}};
    const auto result = apply(json);
    EXPECT_EQ(Field::Applied, result.beats);
    EXPECT_EQ(Field::KeptDjEdit, result.cues);
    EXPECT_EQ(Field::Applied, result.key);
    EXPECT_NEAR(126.0, m_pTrack->getBpm(), 1e-6);
    EXPECT_EQ(key::C_MAJOR, m_pTrack->getKey());
    expectFramesNear(frames(0.5795), m_pTrack->getMainCuePosition());
    EXPECT_EQ(QStringLiteral("drop"), m_pTrack->findHotcueByIndex(0)->getLabel());
}

TEST_F(DjlibImporterTest, analyzerCuesAreNotDjEdits) {
    apply(sidecarJson(3));
    // AnalyzerSilence adds intro/outro cues; that is not a DJ edit.
    m_pTrack->createAndAddCue(mixxx::CueType::Intro,
            Cue::kNoHotCue,
            frames(0.1),
            mixxx::audio::kInvalidFramePos);
    auto json = sidecarJson(4);
    json["cues"] = QJsonArray{QJsonObject{{"type", "main"}, {"pos_s", 1.0}}};
    EXPECT_EQ(Field::Applied, apply(json).cues);
    expectFramesNear(frames(1.0), m_pTrack->getMainCuePosition());
    // Our previous hot cue 1 is gone, the intro cue stays.
    EXPECT_TRUE(hotCues().isEmpty());
    EXPECT_TRUE(m_pTrack->findCueByType(mixxx::CueType::Intro));
}

TEST_F(DjlibImporterTest, loadedDeckDefersBeatsCuesKey) {
    apply(sidecarJson(3));
    auto json = sidecarJson(4);
    json["grid"] = QJsonObject{{"first_beat_s", 0.2}, {"bpm", 126.0}};
    json["key"] = "C";
    json["cues"] = QJsonArray{QJsonObject{{"type", "main"}, {"pos_s", 1.0}}};
    json["crates"] = QJsonArray{QJsonObject{{"id", "crate_b"}, {"name", "Techno"}}};
    json["replaygain"] = QJsonObject{{"gain_db", -6.0}, {"peak", 0.5}};

    const auto loaded = apply(json, true);
    EXPECT_EQ(Field::Deferred, loaded.beats);
    EXPECT_EQ(Field::Deferred, loaded.cues);
    EXPECT_EQ(Field::Deferred, loaded.key);
    EXPECT_TRUE(loaded.hasDeferred());
    EXPECT_NEAR(124.0, m_pTrack->getBpm(), 1e-6);
    EXPECT_EQ(key::A_MINOR, m_pTrack->getKey());
    expectFramesNear(frames(0.5795), m_pTrack->getMainCuePosition());
    // crates and ReplayGain apply at once
    EXPECT_EQ(QStringList{"crate_b"}, managedCratesOfTrack());
    EXPECT_NEAR(db2ratio(-6.0), m_pTrack->getReplayGain().getRatio(), 1e-9);
    EXPECT_EQ(3, m_store.trackState(kDjlibId)->sidecarVersion);

    // Unloaded: the same sidecar completes the import.
    const auto unloaded = apply(json, false);
    EXPECT_FALSE(unloaded.outdated);
    EXPECT_EQ(Field::Applied, unloaded.beats);
    EXPECT_EQ(Field::Applied, unloaded.cues);
    EXPECT_EQ(Field::Applied, unloaded.key);
    EXPECT_FALSE(unloaded.hasDeferred());
    EXPECT_NEAR(126.0, m_pTrack->getBpm(), 1e-6);
    EXPECT_EQ(key::C_MAJOR, m_pTrack->getKey());
    expectFramesNear(frames(1.0), m_pTrack->getMainCuePosition());
    EXPECT_EQ(4, m_store.trackState(kDjlibId)->sidecarVersion);
}

TEST_F(DjlibImporterTest, loadedDeckDefersFirstImport) {
    const auto result = apply(sidecarJson(3), true);
    EXPECT_TRUE(result.hasDeferred());
    EXPECT_FALSE(m_pTrack->getBeats() &&
            m_pTrack->getBeats()->getSubVersion() == DjlibImporter::kBeatsSubVersion);
    EXPECT_EQ(QStringList{"crate_a"}, managedCratesOfTrack());
    const auto state = m_store.trackState(kDjlibId);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(0, state->sidecarVersion);
    EXPECT_EQ(Field::Applied, apply(sidecarJson(3)).beats);
}

TEST_F(DjlibImporterTest, gridChangedOnTheDeckDuringFirstImportIsKept) {
    // Analyzer beats (unlocked) exist when the first sidecar arrives while
    // the track is on a deck.
    const auto analyzed = mixxx::Beats::fromConstTempo(
            m_pTrack->getSampleRate(), frames(0.05), mixxx::Bpm(120.0));
    ASSERT_TRUE(m_pTrack->trySetBeats(analyzed));
    const auto loaded = apply(sidecarJson(3), true);
    EXPECT_EQ(Field::Deferred, loaded.beats);
    EXPECT_FALSE(loaded.baseline.beats.isEmpty());

    // The DJ moves the grid without locking it.
    const auto moved = mixxx::Beats::fromConstTempo(
            m_pTrack->getSampleRate(), frames(0.30), mixxx::Bpm(120.0));
    ASSERT_TRUE(m_pTrack->trySetBeats(moved));
    const auto unloaded = apply(sidecarJson(3), false, loaded.baseline);
    EXPECT_EQ(Field::KeptDjEdit, unloaded.beats);
    EXPECT_EQ(moved, m_pTrack->getBeats());
    EXPECT_FALSE(m_pTrack->isBpmLocked());
    EXPECT_EQ(3, m_store.trackState(kDjlibId)->sidecarVersion);
}

TEST_F(DjlibImporterTest, unchangedGridAfterDeferredFirstImportIsReplaced) {
    const auto analyzed = mixxx::Beats::fromConstTempo(
            m_pTrack->getSampleRate(), frames(0.05), mixxx::Bpm(120.0));
    ASSERT_TRUE(m_pTrack->trySetBeats(analyzed));
    const auto loaded = apply(sidecarJson(3), true);
    // Still loaded: the first baseline is kept.
    const auto again = apply(sidecarJson(3), true, loaded.baseline);
    EXPECT_EQ(loaded.baseline.beats, again.baseline.beats);
    EXPECT_EQ(Field::Applied, apply(sidecarJson(3), false, again.baseline).beats);
    EXPECT_NEAR(124.0, m_pTrack->getBpm(), 1e-6);
}

TEST_F(DjlibImporterTest, analyzerResultsDuringDeferredFirstImportAreReplaced) {
    // Nothing analysed yet when the track goes on the deck.
    ASSERT_FALSE(m_pTrack->getBeats());
    const auto loaded = apply(sidecarJson(3), true);
    EXPECT_EQ(Field::Deferred, loaded.beats);
    EXPECT_EQ(Field::Deferred, loaded.cues);

    // The analyzers finish: beats, and AnalyzerSilence sets the main cue.
    ASSERT_TRUE(m_pTrack->trySetBeats(mixxx::Beats::fromConstTempo(
            m_pTrack->getSampleRate(), frames(0.05), mixxx::Bpm(120.0))));
    m_pTrack->createAndAddCue(mixxx::CueType::N60dBSound,
            Cue::kNoHotCue,
            frames(0.02),
            frames(9.0));
    m_pTrack->setMainCuePosition(frames(0.02));

    const auto unloaded = apply(sidecarJson(3), false, loaded.baseline);
    EXPECT_EQ(Field::Applied, unloaded.beats);
    EXPECT_EQ(Field::Applied, unloaded.cues);
    EXPECT_NEAR(124.0, m_pTrack->getBpm(), 1e-6);
    expectFramesNear(frames(0.5795), m_pTrack->getMainCuePosition());
}

TEST_F(DjlibImporterTest, mainCueMovedOnTheDeckDuringFirstImportIsKept) {
    m_pTrack->setMainCuePosition(frames(0.02));
    const auto loaded = apply(sidecarJson(3), true);
    EXPECT_EQ(Field::Deferred, loaded.cues);

    m_pTrack->setMainCuePosition(frames(2.0)); // the DJ's cue point
    const auto unloaded = apply(sidecarJson(3), false, loaded.baseline);
    EXPECT_EQ(Field::KeptDjEdit, unloaded.cues);
    EXPECT_EQ(Field::Applied, unloaded.beats);
    expectFramesNear(frames(2.0), m_pTrack->getMainCuePosition());
    EXPECT_TRUE(hotCues().isEmpty());
}

TEST_F(DjlibImporterTest, mainCueSetOnTheDeckDuringFirstImportIsKept) {
    // No main cue yet; the analyzer finds the first sound, the DJ then puts
    // the cue somewhere else.
    const auto loaded = apply(sidecarJson(3), true);
    m_pTrack->createAndAddCue(mixxx::CueType::N60dBSound,
            Cue::kNoHotCue,
            frames(0.02),
            frames(9.0));
    m_pTrack->setMainCuePosition(frames(2.0));
    EXPECT_EQ(Field::KeptDjEdit, apply(sidecarJson(3), false, loaded.baseline).cues);
    expectFramesNear(frames(2.0), m_pTrack->getMainCuePosition());
}

TEST_F(DjlibImporterTest, prepareWritesNoRow) {
    const auto result = m_importer.prepare(m_pTrack,
            parse(sidecarJson(3)),
            std::nullopt,
            false,
            QSet<QString>{"crate_a"});
    EXPECT_EQ(Field::Applied, result.beats);
    EXPECT_FALSE(result.stored);
    EXPECT_FALSE(m_store.trackState(kDjlibId).has_value());
    EXPECT_EQ(kDjlibId, result.state.djlibId);
    EXPECT_EQ(m_pTrack->getId(), result.state.trackId);
    EXPECT_EQ(3, result.state.sidecarVersion);
    EXPECT_FALSE(result.state.appliedBeats.isEmpty());

    // The caller's row decides what is outdated and what was applied.
    const auto outdated = m_importer.prepare(m_pTrack,
            parse(sidecarJson(3)),
            result.state,
            false,
            QSet<QString>{"crate_a"});
    EXPECT_TRUE(outdated.outdated);
    auto json = sidecarJson(4);
    json["grid"] = QJsonObject{{"first_beat_s", 0.2}, {"bpm", 126.0}};
    const auto newer = m_importer.prepare(m_pTrack,
            parse(json),
            result.state,
            false,
            QSet<QString>{"crate_a"});
    EXPECT_EQ(Field::Applied, newer.beats);
    EXPECT_NEAR(126.0, m_pTrack->getBpm(), 1e-6);
}

TEST_F(DjlibImporterTest, beatsOnTheSameFrameAreRejected) {
    auto json = sidecarJson(3);
    json["tempo"] = "variable";
    json["grid"] = QJsonValue();
    json["beats_s"] = QJsonArray{0.5, 1.0, 1.5};
    auto sidecar = parse(json);
    // Not reachable through the parser (minimum spacing); the importer must
    // still not hand such positions to Beats::fromBeatPositions().
    sidecar.beatsSeconds = QList<double>{0.5, 0.500001, 1.0};
    const auto result = m_importer.apply(
            m_pTrack, sidecar, m_store, false, QSet<QString>{"crate_a"});
    EXPECT_EQ(Field::Failed, result.beats);
    EXPECT_FALSE(m_pTrack->getBeats());
    EXPECT_FALSE(m_pTrack->isBpmLocked());
}

TEST_F(DjlibImporterTest, unknownTempoLeavesBeatsUntouched) {
    // Analyzer-made beats (unlocked) stay as they are.
    const auto analyzerBeats = mixxx::Beats::fromConstTempo(
            m_pTrack->getSampleRate(), frames(0.05), mixxx::Bpm(120.0));
    ASSERT_TRUE(m_pTrack->trySetBeats(analyzerBeats));
    auto json = sidecarJson(3);
    json["tempo"] = "unknown";
    json["grid"] = QJsonValue();
    json["bpm"] = QJsonValue();
    json["cues"] = QJsonArray();
    const auto result = apply(json);
    EXPECT_EQ(Field::Untouched, result.beats);
    EXPECT_EQ(Field::Untouched, result.cues);
    EXPECT_EQ(analyzerBeats, m_pTrack->getBeats());
    EXPECT_FALSE(m_pTrack->isBpmLocked());
    EXPECT_TRUE(m_store.trackState(kDjlibId)->appliedBeats.isEmpty());

    // A later unknown-tempo sidecar leaves our applied grid alone, too.
    apply(sidecarJson(4));
    const auto ours = m_pTrack->getBeats();
    json["sidecar_version"] = 5;
    EXPECT_EQ(Field::Untouched, apply(json).beats);
    EXPECT_EQ(ours, m_pTrack->getBeats());
    EXPECT_TRUE(m_pTrack->isBpmLocked());
}

TEST_F(DjlibImporterTest, lockedBeatsWithoutFingerprintAreDjWork) {
    const auto djBeats = mixxx::Beats::fromConstTempo(
            m_pTrack->getSampleRate(), frames(0.05), mixxx::Bpm(120.0));
    ASSERT_TRUE(m_pTrack->trySetBeats(djBeats));
    m_pTrack->setBpmLocked(true);
    EXPECT_EQ(Field::KeptDjEdit, apply(sidecarJson(3)).beats);
    EXPECT_EQ(djBeats, m_pTrack->getBeats());
}

TEST_F(DjlibImporterTest, fingerprintsSurviveSaveAndReload) {
    apply(sidecarJson(3));
    const TrackId trackId = m_pTrack->getId();
    trackCollectionManager()->saveTrack(m_pTrack);
    m_pTrack.reset(); // evicted from the cache
    m_pTrack = getOrAddTrackByLocation(m_tempDir.filePath(kTrackFile));
    ASSERT_TRUE(m_pTrack);
    EXPECT_EQ(trackId, m_pTrack->getId());
    const auto state = m_store.trackState(kDjlibId);
    EXPECT_TRUE(DjlibImporter::fingerprintsMatch(
            state->appliedBeats, DjlibImporter::beatsFingerprint(m_pTrack->getBeats())))
            << state->appliedBeats.toStdString() << " vs "
            << DjlibImporter::beatsFingerprint(m_pTrack->getBeats()).toStdString();
    EXPECT_TRUE(DjlibImporter::fingerprintsMatch(
            state->appliedCues, DjlibImporter::cuesFingerprint(*m_pTrack)))
            << state->appliedCues.toStdString() << " vs "
            << DjlibImporter::cuesFingerprint(*m_pTrack).toStdString();
    auto json = sidecarJson(4);
    json["key"] = "C";
    const auto result = apply(json);
    EXPECT_EQ(Field::Applied, result.beats);
    EXPECT_EQ(Field::Applied, result.cues);
    EXPECT_EQ(Field::Applied, result.key);
}

class DjlibAnalyzerKeyGuardTest : public DjlibImporterTest {
  protected:
    bool keyAnalyzerWouldRun() {
        KeyDetectionSettings settings(config());
        settings.setReanalyzeWhenSettingsChange(true);
        AnalyzerKey analyzer(settings);
        const auto sampleRate = mixxx::audio::SampleRate(44100);
        const bool run = analyzer.initialize(AnalyzerTrack(m_pTrack),
                sampleRate,
                mixxx::audio::ChannelCount(2),
                441000);
        analyzer.cleanup();
        return run;
    }
};

TEST_F(DjlibAnalyzerKeyGuardTest, djlibKeyIsNotReanalyzed) {
    apply(sidecarJson(1));
    EXPECT_EQ(DjlibImporter::kKeySubVersion, m_pTrack->getKeys().getSubVersion());
    EXPECT_FALSE(keyAnalyzerWouldRun());

    // The tag survives a save and reload.
    trackCollectionManager()->saveTrack(m_pTrack);
    m_pTrack.reset();
    m_pTrack = getOrAddTrackByLocation(m_tempDir.filePath(kTrackFile));
    ASSERT_TRUE(m_pTrack);
    EXPECT_EQ(DjlibImporter::kKeySubVersion, m_pTrack->getKeys().getSubVersion());
    EXPECT_FALSE(keyAnalyzerWouldRun());
}

TEST_F(DjlibAnalyzerKeyGuardTest, keyImportedDuringAnalysisIsNotOverwritten) {
    // The analysis starts on a track without key ...
    KeyDetectionSettings settings(config());
    AnalyzerKey analyzer(settings);
    const SINT kFrames = 44100;
    ASSERT_TRUE(analyzer.initialize(AnalyzerTrack(m_pTrack),
            mixxx::audio::SampleRate(44100),
            mixxx::audio::ChannelCount(2),
            kFrames));
    // ... the sidecar arrives while it runs ...
    apply(sidecarJson(1));
    ASSERT_EQ(DjlibImporter::kKeySubVersion, m_pTrack->getKeys().getSubVersion());
    const auto key = m_pTrack->getKey();
    // ... and the analysis finishes.
    const std::vector<CSAMPLE> samples(kFrames * 2, 0.0f);
    ASSERT_TRUE(analyzer.processSamples(samples.data(), static_cast<SINT>(samples.size())));
    analyzer.storeResults(m_pTrack);
    analyzer.cleanup();
    EXPECT_EQ(DjlibImporter::kKeySubVersion, m_pTrack->getKeys().getSubVersion());
    EXPECT_EQ(key, m_pTrack->getKey());
}

TEST_F(DjlibAnalyzerKeyGuardTest, otherKeysBehaveAsUpstream) {
    // An outdated analyzer key and an ordinary file-tag key are re-analysed
    // with ReanalyzeWhenSettingsChange, as in stock Mixxx.
    m_pTrack->setKey(key::A_MINOR, key::ANALYZER);
    EXPECT_TRUE(keyAnalyzerWouldRun());
    m_pTrack->setKey(key::A_MINOR, key::FILE_METADATA);
    EXPECT_TRUE(keyAnalyzerWouldRun());
}

TEST_F(DjlibImporterTest, userKeyWithoutFingerprintIsKept) {
    m_pTrack->setKey(key::C_MAJOR, key::USER);
    EXPECT_EQ(Field::KeptDjEdit, apply(sidecarJson(1)).key);
    EXPECT_EQ(key::C_MAJOR, m_pTrack->getKey());
}
