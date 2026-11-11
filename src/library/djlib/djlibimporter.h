#pragma once

#include <QSet>
#include <QString>

#include "library/djlib/djlibsidecar.h"
#include "library/djlib/djlibstore.h"
#include "track/beats.h"
#include "track/track_decl.h"

class DjlibCrateSync;

/// Applies one parsed sidecar to one library track (research §2-5, §7).
///
/// - Beats: constant -> `Beats::fromConstTempo`, variable ->
///   `Beats::fromBeatPositions`, subVersion "djlib/1", BPM locked
///   (unlock-set-relock). Unknown tempo: beats are not touched.
/// - Cues: main cue + hot cues (sidecar `number` 1 -> hot cue index 0) with
///   their labels. A sidecar without cues leaves the cues alone.
/// - Key: `guessKeyFromText(key)` as FILE_METADATA keys with sub-version
///   "djlib/1" (`setKeys`), which the AnalyzerKey guard keeps. Null: untouched.
/// - ReplayGain: ratio = `db2ratio(gain_db)`, peak linear. Null: untouched.
/// - Crates: membership in managed crates = sidecar crates ∩ `allowedCrateIds`
///   (the ids of the current crates.json).
///
/// "DJ edits win": after applying beats, cues or key, a fingerprint of the
/// track's resulting value is stored in `djlib_tracks`. On a later sidecar a
/// field whose current value no longer matches its fingerprint was edited by
/// the DJ and is kept (the old fingerprint stays, so it keeps winning). Without
/// a fingerprint, locked beats, existing hot cues and a USER key count as DJ
/// work too. Crates and ReplayGain always update.
///
/// If the track is loaded in a deck (`trackLoaded`), or its sample rate is
/// still unknown (beats/cues only; probe and call again), those fields are
/// deferred: crates/ReplayGain are applied, but the stored `sidecar_version`
/// is not raised, so calling `apply()` again with the same sidecar later
/// completes the import. A first import (no fingerprint yet) deferred on a
/// loaded deck also reports the fingerprints of the track's beats and cues at
/// that time (`Baseline`). Passed in again on the next call they tell what the
/// DJ changed meanwhile: beats that existed and are different now, or a main
/// cue that has moved, are kept. Beats that only appeared meanwhile and a
/// main cue that went from unset to the first sound are the analyzers' and
/// are replaced.
///
/// Sidecars whose `sidecar_version` is not greater than the last fully applied
/// one are ignored (`outdated`).
///
/// `prepare()` changes the track and the crates but writes no `djlib_tracks`
/// row: the caller saves the track (`TrackCollectionManager::saveTrack`) and
/// stores `ApplyResult::state` only if that worked. Otherwise a failed save
/// would leave fingerprints of values the library never got, and the next
/// sidecar would take the old values for DJ edits. `apply()` does both steps
/// without saving the track (tests, callers that save themselves).
///
/// Synchronous, main thread only.
class DjlibImporter {
  public:
    static const QString kBeatsSubVersion; // "djlib/1"
    /// Keys sub-version of imported keys; AnalyzerKey never re-analyses them.
    static const QString kKeySubVersion; // "djlib/1"

    enum class Field {
        Untouched,  // nothing to apply (null/unknown/empty in the sidecar)
        Applied,    // set from the sidecar
        KeptDjEdit, // edited by the DJ since our last apply: kept
        Deferred,   // track loaded in a deck or sample rate unknown: retry later
        Failed,     // the sidecar data was rejected by Mixxx
    };

    /// Fingerprints taken when a first import was deferred on a loaded deck
    /// (empty: none taken).
    struct Baseline {
        QString beats;
        QString cues;
    };

    struct ApplyResult {
        bool outdated = false;        // sidecar_version <= stored: nothing done
        bool needsSampleRate = false; // beats/cues deferred until the sample rate is known
        bool cratesApplied = false;
        bool replayGainApplied = false;
        Field beats = Field::Untouched;
        Field cues = Field::Untouched;
        Field key = Field::Untouched;
        bool stored = false; // djlib_tracks row written (apply() only)
        /// The row to store once the track is saved (unless `outdated`).
        DjlibStore::TrackState state;
        /// For the next call with the same sidecar (see `Baseline`).
        Baseline baseline;

        /// True if beats, cues or key were deferred: call apply() again
        /// (same sidecar) once the deck unloads / the sample rate is known.
        bool hasDeferred() const {
            return beats == Field::Deferred || cues == Field::Deferred ||
                    key == Field::Deferred;
        }
    };

    explicit DjlibImporter(DjlibCrateSync* pCrateSync);

    /// `existing`: the track's `djlib_tracks` row, if any.
    ApplyResult prepare(const TrackPointer& pTrack,
            const DjlibSidecar& sidecar,
            const std::optional<DjlibStore::TrackState>& existing,
            bool trackLoaded,
            const QSet<QString>& allowedCrateIds,
            const Baseline& baseline = Baseline());

    /// `prepare()` with the stored row, then stores the new row.
    ApplyResult apply(const TrackPointer& pTrack,
            const DjlibSidecar& sidecar,
            DjlibStore& store,
            bool trackLoaded,
            const QSet<QString>& allowedCrateIds,
            const Baseline& baseline = Baseline());

    /// Fingerprints of the track's current values (exposed for tests).
    static QString beatsFingerprint(const mixxx::BeatsPointer& pBeats);
    static QString cuesFingerprint(const Track& track);

    /// Compares two fingerprints of the same kind with a tolerance
    /// (0.5 frames for positions, 1e-6 for BPMs).
    static bool fingerprintsMatch(const QString& lhs, const QString& rhs);

  private:
    DjlibCrateSync* const m_pCrateSync;
};
