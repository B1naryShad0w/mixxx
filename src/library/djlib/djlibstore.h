#pragma once

#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <optional>

#include "library/trackset/crate/crateid.h"
#include "track/trackid.h"

/// djlib state kept in two own tables of mixxxdb.sqlite (research §6):
///  - `djlib_crates`: djlib crate id (crates.json) <-> Mixxx crate id. Only
///    crates in this table are ever touched by djlib.
///  - `djlib_tracks`: per djlib track id: Mixxx track id, file, last applied
///    sidecar/package version, fingerprints of what was applied (§7) and a
///    parsed-but-not-yet-applicable sidecar (`pending_json`).
///
/// The tables are created with `CREATE TABLE IF NOT EXISTS` (no schema.xml
/// revision, so rebases on upstream never collide). No REFERENCES clauses:
/// the mapping must never block a DJ deleting a crate or purging a track;
/// stale rows are cleaned up by DjlibCrateSync / the service instead.
///
/// Main thread only (uses the main-thread DB connection).
class DjlibStore {
  public:
    struct CrateMapping {
        QString djlibId;
        CrateId crateId;
        QString name; // djlib name from crates.json (not the possibly suffixed Mixxx name)
    };

    struct TrackState {
        QString djlibId;
        TrackId trackId; // invalid while the track isn't in the library yet
        QString file;
        qint64 sidecarVersion = 0; // last applied; 0 = never applied
        std::optional<qint64> packageVersion;
        QString appliedBeats; // fingerprints, empty = nothing applied
        QString appliedCues;
        std::optional<int> appliedKey; // mixxx::track::io::key::ChromaticKey
        std::optional<QByteArray> pendingJson;
        qint64 updatedAt = 0; // ms since epoch, set on every upsert
    };

    explicit DjlibStore(const QSqlDatabase& database);

    /// Creates the tables if missing. Idempotent.
    bool initialize();

    // Crates
    QList<CrateMapping> crateMappings() const;
    std::optional<CrateMapping> crateMapping(const QString& djlibId) const;
    std::optional<CrateMapping> crateMappingByCrateId(CrateId crateId) const;
    bool upsertCrateMapping(const CrateMapping& mapping);
    bool removeCrateMapping(const QString& djlibId);
    bool removeCrateMappingByCrateId(CrateId crateId);

    // Tracks
    QList<TrackState> trackStates() const;
    std::optional<TrackState> trackState(const QString& djlibId) const;
    std::optional<TrackState> trackStateByTrackId(TrackId trackId) const;
    /// Inserts or replaces the whole row; sets `updated_at`.
    bool upsertTrackState(const TrackState& state);
    /// Sets or clears (std::nullopt) `pending_json` of an existing row.
    bool setPendingJson(const QString& djlibId, const std::optional<QByteArray>& json);
    bool removeTrackState(const QString& djlibId);
    /// For `TrackCollection::tracksRemoved` (purge).
    bool removeTrackStatesByTrackIds(const QList<TrackId>& trackIds);

  private:
    QList<TrackState> selectTrackStates(const QString& where, const QVariant& value) const;

    QSqlDatabase m_database;
};
