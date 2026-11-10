#include "library/djlib/djlibimporter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cmath>

#include "library/djlib/djlibcratesync.h"
#include "library/djlib/djlibstore.h"
#include "proto/keys.pb.h"
#include "track/cue.h"
#include "track/keyfactory.h"
#include "track/keys.h"
#include "track/keyutils.h"
#include "track/replaygain.h"
#include "track/track.h"
#include "util/assert.h"
#include "util/defs.h"
#include "util/logger.h"
#include "util/math.h"

namespace {

const mixxx::Logger kLogger("DjlibImporter");

using Field = DjlibImporter::Field;

// Fingerprint tolerances: positions in frames, tempos in BPM.
constexpr double kFrameTolerance = 0.5;
constexpr double kBpmTolerance = 1e-6;

QJsonValue framesToJson(mixxx::audio::FramePos position) {
    return position.isValid() ? QJsonValue(position.value()) : QJsonValue();
}

QString toCompactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool jsonMatches(const QJsonValue& lhs, const QJsonValue& rhs, double tolerance) {
    if (lhs.isDouble() && rhs.isDouble()) {
        return std::abs(lhs.toDouble() - rhs.toDouble()) <= tolerance;
    }
    if (lhs.type() != rhs.type()) {
        return false;
    }
    if (lhs.isArray()) {
        const QJsonArray l = lhs.toArray();
        const QJsonArray r = rhs.toArray();
        if (l.size() != r.size()) {
            return false;
        }
        for (qsizetype i = 0; i < l.size(); ++i) {
            if (!jsonMatches(l.at(i), r.at(i), tolerance)) {
                return false;
            }
        }
        return true;
    }
    if (lhs.isObject()) {
        const QJsonObject l = lhs.toObject();
        const QJsonObject r = rhs.toObject();
        if (l.keys() != r.keys()) {
            return false;
        }
        for (auto it = l.constBegin(); it != l.constEnd(); ++it) {
            const double fieldTolerance =
                    it.key() == QLatin1String("bpm") ? kBpmTolerance : tolerance;
            if (!jsonMatches(it.value(), r.value(it.key()), fieldTolerance)) {
                return false;
            }
        }
        return true;
    }
    return lhs == rhs;
}

/// Any cue on a hot cue slot (hot cue, saved loop, ...): what a DJ edits.
/// The main cue is fingerprinted separately; Intro/Outro/N60dBSound are
/// analyzer-made (research §3) and ignored.
bool isHotCue(const CuePointer& pCue) {
    return pCue->getType() != mixxx::CueType::MainCue &&
            pCue->getHotCue() != Cue::kNoHotCue;
}

/// Keys has no source accessor (core header kept unchanged): read it from
/// the serialized KeyMap.
mixxx::track::io::key::Source keySource(const Keys& keys) {
    const QByteArray bytes = keys.toByteArray();
    mixxx::track::io::key::KeyMap keyMap;
    if (!keyMap.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size()))) {
        return mixxx::track::io::key::ANALYZER;
    }
    return keyMap.source();
}

bool hasHotCues(const Track& track) {
    const auto cues = track.getCuePoints();
    return std::any_of(cues.cbegin(), cues.cend(), isHotCue);
}

mixxx::BeatsPointer makeBeats(const DjlibSidecar& sidecar, mixxx::audio::SampleRate sampleRate) {
    const double rate = sampleRate.toDouble();
    switch (sidecar.tempo) {
    case DjlibSidecar::Tempo::Constant:
        VERIFY_OR_DEBUG_ASSERT(sidecar.grid) {
            return nullptr;
        }
        // Whole frames: the DB format stores the grid anchor floored to a
        // frame, which would otherwise change the beats fingerprint on reload.
        return mixxx::Beats::fromConstTempo(sampleRate,
                mixxx::audio::FramePos(sidecar.grid->firstBeatSeconds * rate)
                        .toNearestFrameBoundary(),
                mixxx::Bpm(sidecar.grid->bpm),
                DjlibImporter::kBeatsSubVersion);
    case DjlibSidecar::Tempo::Variable: {
        VERIFY_OR_DEBUG_ASSERT(sidecar.beatsSeconds) {
            return nullptr;
        }
        QVector<mixxx::audio::FramePos> positions;
        positions.reserve(sidecar.beatsSeconds->size());
        for (const double seconds : std::as_const(*sidecar.beatsSeconds)) {
            positions.append(mixxx::audio::FramePos(seconds * rate).toNearestFrameBoundary());
        }
        return mixxx::Beats::fromBeatPositions(
                sampleRate, positions, DjlibImporter::kBeatsSubVersion);
    }
    case DjlibSidecar::Tempo::Unknown:
        break;
    }
    return nullptr;
}

/// trySetBeats() discards new beats while the BPM is locked (research §2):
/// unlock, set, relock.
bool applyBeats(Track* pTrack, const mixxx::BeatsPointer& pBeats) {
    pTrack->setBpmLocked(false);
    pTrack->trySetBeats(pBeats);
    pTrack->setBpmLocked(true);
    return pTrack->getBeats() == pBeats;
}

void applyCues(Track* pTrack,
        const QList<DjlibSidecar::Cue>& cues,
        mixxx::audio::SampleRate sampleRate) {
    const double rate = sampleRate.toDouble();
    QList<int> hotCueIndices;
    for (const auto& cue : cues) {
        const auto position = mixxx::audio::FramePos(cue.posSeconds * rate);
        switch (cue.type) {
        case DjlibSidecar::Cue::Type::Main:
            pTrack->setMainCuePosition(position);
            break;
        case DjlibSidecar::Cue::Type::HotCue: {
            // Sidecar numbers are 1-based, Mixxx hot cue indices 0-based.
            const int index = mixxx::kFirstHotCueIndex + cue.number - 1;
            if (index >= mixxx::kFirstHotCueIndex + kMaxNumberOfHotcues) {
                break; // beyond Mixxx's hot cues
            }
            if (hotCueIndices.contains(index)) {
                break; // duplicate number: the first one wins
            }
            hotCueIndices.append(index);
            CuePointer pCue = pTrack->findHotcueByIndex(index);
            if (pCue && pCue->getType() != mixxx::CueType::HotCue) {
                pTrack->removeCue(pCue);
                pCue.reset();
            }
            if (pCue) {
                pCue->setStartAndEndPosition(position, mixxx::audio::kInvalidFramePos);
            } else {
                pCue = pTrack->createAndAddCue(mixxx::CueType::HotCue,
                        index,
                        position,
                        mixxx::audio::kInvalidFramePos);
            }
            if (pCue) {
                pCue->setLabel(cue.label);
            }
            break;
        }
        }
    }
    // Hot cues we set earlier that the sidecar no longer lists. We only get
    // here if the cues are unchanged since our last apply (or there were no
    // hot cues at all), so every remaining hot cue is ours.
    const auto cuePoints = pTrack->getCuePoints();
    for (const auto& pCue : cuePoints) {
        if (isHotCue(pCue) && !hotCueIndices.contains(pCue->getHotCue())) {
            pTrack->removeCue(pCue);
        }
    }
}

} // namespace

const QString DjlibImporter::kBeatsSubVersion = QStringLiteral("djlib/1");
const QString DjlibImporter::kKeySubVersion = QStringLiteral("djlib/1");

DjlibImporter::DjlibImporter(DjlibCrateSync* pCrateSync)
        : m_pCrateSync(pCrateSync) {
}

// static
QString DjlibImporter::beatsFingerprint(const mixxx::BeatsPointer& pBeats) {
    if (!pBeats) {
        return QStringLiteral("{}");
    }
    double markerSum = 0;
    for (const auto& marker : pBeats->getMarkers()) {
        markerSum += marker.position().value();
    }
    const mixxx::Bpm bpm = pBeats->getLastMarkerBpm();
    return toCompactJson(QJsonObject{
            {"v", pBeats->getVersion()},
            {"s", pBeats->getSubVersion()},
            {"sr", pBeats->getSampleRate().toDouble()},
            {"first", framesToJson(pBeats->firstBeat())},
            {"n", static_cast<qint64>(pBeats->getMarkers().size())},
            {"sum", markerSum},
            {"last", framesToJson(pBeats->getLastMarkerPosition())},
            {"bpm", bpm.isValid() ? QJsonValue(bpm.value()) : QJsonValue()},
    });
}

// static
QString DjlibImporter::cuesFingerprint(const Track& track) {
    QList<CuePointer> hotCues;
    const auto cuePoints = track.getCuePoints();
    for (const auto& pCue : cuePoints) {
        if (isHotCue(pCue)) {
            hotCues.append(pCue);
        }
    }
    std::sort(hotCues.begin(), hotCues.end(), [](const CuePointer& lhs, const CuePointer& rhs) {
        return lhs->getHotCue() < rhs->getHotCue();
    });
    QJsonArray hot;
    for (const auto& pCue : std::as_const(hotCues)) {
        hot.append(QJsonObject{
                {"i", pCue->getHotCue()},
                {"t", static_cast<int>(pCue->getType())},
                {"start", framesToJson(pCue->getPosition())},
                {"end", framesToJson(pCue->getEndPosition())},
                {"label", pCue->getLabel()},
        });
    }
    return toCompactJson(QJsonObject{
            {"main", framesToJson(track.getMainCuePosition())},
            {"hot", hot},
    });
}

// static
bool DjlibImporter::fingerprintsMatch(const QString& lhs, const QString& rhs) {
    if (lhs == rhs) {
        return true;
    }
    const QJsonDocument l = QJsonDocument::fromJson(lhs.toUtf8());
    const QJsonDocument r = QJsonDocument::fromJson(rhs.toUtf8());
    if (!l.isObject() || !r.isObject()) {
        return false;
    }
    return jsonMatches(QJsonValue(l.object()), QJsonValue(r.object()), kFrameTolerance);
}

DjlibImporter::ApplyResult DjlibImporter::apply(const TrackPointer& pTrack,
        const DjlibSidecar& sidecar,
        DjlibStore& store,
        bool trackLoaded,
        const QSet<QString>& allowedCrateIds) {
    ApplyResult result;
    VERIFY_OR_DEBUG_ASSERT(pTrack && !sidecar.trackId.isEmpty()) {
        return result;
    }

    const auto existing = store.trackState(sidecar.trackId);
    if (existing && sidecar.sidecarVersion <= existing->sidecarVersion) {
        result.outdated = true;
        return result;
    }
    DjlibStore::TrackState state = existing.value_or(DjlibStore::TrackState());
    state.djlibId = sidecar.trackId;
    state.trackId = pTrack->getId();
    state.file = sidecar.file;
    state.packageVersion = sidecar.packageVersion;

    // Crates: always; sidecar crates ∩ crates.json.
    if (m_pCrateSync && state.trackId.isValid()) {
        QStringList crateIds;
        for (const auto& crate : sidecar.crates) {
            if (allowedCrateIds.contains(crate.id) && !crateIds.contains(crate.id)) {
                crateIds.append(crate.id);
            }
        }
        result.cratesApplied = m_pCrateSync->setMembership(state.trackId, crateIds);
    }

    // ReplayGain: always, unless null.
    if (sidecar.replayGain) {
        pTrack->setReplayGain(mixxx::ReplayGain(
                db2ratio(sidecar.replayGain->gainDb),
                static_cast<CSAMPLE>(sidecar.replayGain->peak)));
        result.replayGainApplied = true;
    }

    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();

    // Beats. Without a fingerprint, locked beats are DJ work (research §2).
    if (sidecar.tempo != DjlibSidecar::Tempo::Unknown) {
        const auto pCurrent = pTrack->getBeats();
        const bool edited = state.appliedBeats.isEmpty()
                ? (pCurrent && pTrack->isBpmLocked())
                : !fingerprintsMatch(state.appliedBeats, beatsFingerprint(pCurrent));
        if (trackLoaded) {
            result.beats = Field::Deferred;
        } else if (edited) {
            result.beats = Field::KeptDjEdit;
        } else if (!sampleRate.isValid()) {
            result.beats = Field::Deferred;
            result.needsSampleRate = true;
        } else {
            const auto pBeats = makeBeats(sidecar, sampleRate);
            if (pBeats && applyBeats(pTrack.get(), pBeats)) {
                result.beats = Field::Applied;
                state.appliedBeats = beatsFingerprint(pTrack->getBeats());
            } else {
                kLogger.warning() << "Beats rejected for" << sidecar.trackId;
                result.beats = Field::Failed;
            }
        }
    }

    // Cues. Without a fingerprint, existing hot cues are DJ work.
    if (!sidecar.cues.isEmpty()) {
        const bool edited = state.appliedCues.isEmpty()
                ? hasHotCues(*pTrack)
                : !fingerprintsMatch(state.appliedCues, cuesFingerprint(*pTrack));
        if (trackLoaded) {
            result.cues = Field::Deferred;
        } else if (edited) {
            result.cues = Field::KeptDjEdit;
        } else if (!sampleRate.isValid()) {
            result.cues = Field::Deferred;
            result.needsSampleRate = true;
        } else {
            applyCues(pTrack.get(), sidecar.cues, sampleRate);
            result.cues = Field::Applied;
            state.appliedCues = cuesFingerprint(*pTrack);
        }
    }

    // Key. Without a fingerprint, a key the user typed is DJ work.
    if (sidecar.key) {
        const auto newKey = KeyUtils::guessKeyFromText(*sidecar.key);
        const auto currentKey = pTrack->getKey();
        const bool edited = state.appliedKey
                ? static_cast<int>(currentKey) != *state.appliedKey
                : (currentKey != mixxx::track::io::key::INVALID &&
                          keySource(pTrack->getKeys()) == mixxx::track::io::key::USER);
        if (newKey == mixxx::track::io::key::INVALID) {
            kLogger.warning() << "Unparsable key" << *sidecar.key << "for" << sidecar.trackId;
            result.key = Field::Failed;
        } else if (trackLoaded) {
            result.key = Field::Deferred;
        } else if (edited) {
            result.key = Field::KeptDjEdit;
        } else {
            // Tagged so the AnalyzerKey guard leaves exactly these keys alone.
            Keys keys = KeyFactory::makeBasicKeys(newKey, mixxx::track::io::key::FILE_METADATA);
            keys.setSubVersion(kKeySubVersion);
            pTrack->setKeys(keys);
            result.key = Field::Applied;
            state.appliedKey = static_cast<int>(newKey);
        }
    }

    // Only a complete import raises the stored version, so a deferred one is
    // completed by calling apply() again with the same sidecar.
    if (!result.hasDeferred()) {
        state.sidecarVersion = sidecar.sidecarVersion;
    }
    result.stored = store.upsertTrackState(state);
    return result;
}
