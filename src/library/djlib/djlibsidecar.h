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
/// type; every number finite; times >= 0; BPMs > 0; `beats_s` strictly
/// ascending; cue types "main"/"hotcue" (hot cue `number` >= 1). Nullable fields
/// may also be absent (treated as null). Unknown fields are ignored. Beat data
/// that doesn't belong to the tempo (grid for variable, beats for constant,
/// both for unknown) is dropped.
struct DjlibSidecar {
    enum class Tempo {
        Constant, // use `grid`, lock the BPM
        Variable, // use `beatsSeconds`
        Unknown,  // leave beats to Mixxx's own analysis
    };

    struct Grid {
        double firstBeatSeconds;
        double bpm;
    };

    struct ReplayGain {
        double gainDb;
        double peak; // linear true peak, 1.0 = 0 dBTP
    };

    struct Cue {
        enum class Type {
            Main,
            HotCue,
        };
        Type type;
        int number; // 1-based hot cue number; 0 for the main cue
        double posSeconds;
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
    QList<djlib::CrateRef> crates; // all crates of the track, unfiltered
    Tempo tempo = Tempo::Unknown;
    std::optional<double> bpm;
    std::optional<QString> bpmSource;
    std::optional<Grid> grid;                 // set iff tempo == Constant
    std::optional<QList<double>> beatsSeconds; // set iff tempo == Variable, non-empty
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
