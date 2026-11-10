#pragma once

#include <QSet>
#include <QString>

#include "library/djlib/djlibsidecar.h"
#include "track/beats.h"
#include "track/track_decl.h"

class DjlibCrateSync;
class DjlibStore;

/// Applies one parsed sidecar to one library track (research §2-5, §7).
///
/// - Beats: constant -> `Beats::fromConstTempo`, variable ->
///   `Beats::fromBeatPositions`, subVersion "djlib/1", BPM locked
///   (unlock-set-relock). Unknown tempo: beats are not touched.
/// - Cues: main cue + hot cues (sidecar `number` 1 -> hot cue index 0) with
///   their labels. A sidecar without cues leaves the cues alone.
/// - Key: `setKey(guessKeyFromText(key), FILE_METADATA)`. Null key: untouched.
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
/// completes the import.
///
/// Sidecars whose `sidecar_version` is not greater than the last fully applied
/// one are ignored (`outdated`).
///
/// Synchronous, main thread only. The caller saves the track afterwards
/// (`TrackCollectionManager::saveTrack`).
class DjlibImporter {
  public:
    static const QString kBeatsSubVersion; // "djlib/1"

    enum class Field {
        Untouched,  // nothing to apply (null/unknown/empty in the sidecar)
        Applied,    // set from the sidecar
        KeptDjEdit, // edited by the DJ since our last apply: kept
        Deferred,   // track loaded in a deck or sample rate unknown: retry later
        Failed,     // the sidecar data was rejected by Mixxx
    };

    struct ApplyResult {
        bool outdated = false;        // sidecar_version <= stored: nothing done
        bool needsSampleRate = false; // beats/cues deferred until the sample rate is known
        bool cratesApplied = false;
        bool replayGainApplied = false;
        Field beats = Field::Untouched;
        Field cues = Field::Untouched;
        Field key = Field::Untouched;
        bool stored = false; // djlib_tracks row written

        /// True if beats, cues or key were deferred: call apply() again
        /// (same sidecar) once the deck unloads / the sample rate is known.
        bool hasDeferred() const {
            return beats == Field::Deferred || cues == Field::Deferred ||
                    key == Field::Deferred;
        }
    };

    explicit DjlibImporter(DjlibCrateSync* pCrateSync);

    ApplyResult apply(const TrackPointer& pTrack,
            const DjlibSidecar& sidecar,
            DjlibStore& store,
            bool trackLoaded,
            const QSet<QString>& allowedCrateIds);

    /// Fingerprints of the track's current values (exposed for tests).
    static QString beatsFingerprint(const mixxx::BeatsPointer& pBeats);
    static QString cuesFingerprint(const Track& track);

    /// Compares two fingerprints of the same kind with a tolerance
    /// (0.5 frames for positions, 1e-6 for BPMs).
    static bool fingerprintsMatch(const QString& lhs, const QString& rhs);

  private:
    DjlibCrateSync* const m_pCrateSync;
};
