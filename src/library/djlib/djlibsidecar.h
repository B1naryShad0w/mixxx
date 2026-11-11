#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace djlib {

/// A djlib crate reference: opaque server id plus display name.
/// Used by sidecars (`crates`) and by `_djlib/crates.json`.
struct CrateRef {
    QString id;
    QString name;

    friend bool operator==(const CrateRef& lhs, const CrateRef& rhs) {
        return lhs.id == rhs.id && lhs.name == rhs.name;
    }
    friend bool operator!=(const CrateRef& lhs, const CrateRef& rhs) {
        return !(lhs == rhs);
    }
};

/// Limits: a broken or hostile file must never hurt Mixxx. Files and values
/// beyond them are invalid (cue labels are cut instead).
constexpr qint64 kMaxJsonBytes = 4 * 1024 * 1024; // sidecar or crates.json
constexpr double kMaxSeconds = 24 * 60 * 60.0;    // any position
constexpr double kMinBpm = 10.0;
constexpr double kMaxBpm = 500.0; // mixxx::Bpm::kValueMax
constexpr double kMaxGainDb = 60.0; // +-
constexpr double kMaxPeak = 100.0;
constexpr qsizetype kMaxBeats = 200000;
constexpr double kMinBeatSeconds = 0.01; // between two beats of `beats_s`
constexpr qsizetype kMaxCues = 1000;
constexpr qsizetype kMaxCrates = 10000;
constexpr qsizetype kMaxLabelChars = 256;

/// Parses `<library>/_djlib/crates.json` (`{"schema": 1, "crates": [{"id", "name"}, ...]}`).
/// Returns std::nullopt and sets *pError (if given) when the document is invalid.
/// Unknown fields are ignored. Pure function, safe on any thread.
std::optional<QList<CrateRef>> parseCratesJson(
        const QByteArray& json, QString* pError = nullptr);

} // namespace djlib

/// A parsed and validated djlib sidecar (`<file minus .stem.mp4>.djlib.json`),
/// schema 1 of docs/sync-contract.md "Sidecar". Times are in seconds.
///
/// Validation: schema must be 1; required fields must be present with the right
/// type; every number finite; times >= 0 and at most 24 h; BPMs within
/// 10..500; `beats_s` at least two beats, ascending by at least 10 ms;
/// ReplayGain within +-60 dB, peak at most 100; at most 4 MiB of JSON (more
/// limits: `djlib::kMax...`); known cue types "main"/"hotcue" (hot cue `number` >= 1) are
/// validated, cues of unknown types are skipped and an unknown tempo value is
/// treated as "unknown" (forward compatibility within schema 1). Nullable fields
/// may also be absent (treated as null). Unknown fields are ignored. Beat data
/// that doesn't belong to the tempo (grid for variable, beats for constant,
/// both for unknown) is dropped.
struct DjlibSidecar {
    enum class Tempo {
        Constant, // use `grid`, lock the BPM
        Variable, // use `beatsSeconds`
        Unknown,  // leave beats to Mixxx's own analysis (also any unknown value)
    };

    struct Grid {
        double firstBeatSeconds = 0;
        double bpm = 0;
    };

    struct ReplayGain {
        double gainDb = 0;
        double peak = 0; // linear true peak, 1.0 = 0 dBTP
    };

    struct Cue {
        enum class Type {
            Main,
            HotCue,
        };
        Type type = Type::Main;
        int number = 0; // 1-based hot cue number; 0 for the main cue
        double posSeconds = 0;
        QString label; // empty if none
    };

    int schema = 0;
    QString trackId;
    std::optional<QString> tidalId;
    std::optional<QString> isrc;
    qint64 sidecarVersion = 0;
    qint64 packageVersion = 0;
    QString file;
    QString sha256;
    // All crates of the track, unfiltered. Names may be empty and ids may
    // repeat; DjlibCrateSync dedupes by id and falls back to the id as name.
    QList<djlib::CrateRef> crates;
    Tempo tempo = Tempo::Unknown;
    std::optional<double> bpm;
    std::optional<QString> bpmSource;
    std::optional<Grid> grid;                 // set iff tempo == Constant
    std::optional<QList<double>> beatsSeconds; // set iff tempo == Variable, >= 2 beats
    std::optional<double> firstDownbeatSeconds;
    std::optional<QString> key; // Mixxx traditional notation, e.g. "Am", "F#"
    std::optional<ReplayGain> replayGain;
    QList<Cue> cues;
    QStringList flags; // open set; unknown flags are kept, callers ignore them

    /// Parses and validates a sidecar. Returns std::nullopt and sets *pError
    /// (if given) on any error. Pure function, safe on any thread.
    static std::optional<DjlibSidecar> parse(
            const QByteArray& json, QString* pError = nullptr);
};
