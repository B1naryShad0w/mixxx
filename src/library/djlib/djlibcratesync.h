#pragma once

#include <QList>
#include <QObject>
#include <QStringList>

#include "library/djlib/djlibsidecar.h"
#include "library/trackset/crate/crateid.h"
#include "track/trackid.h"

class DjlibStore;
class TrackCollection;

/// Keeps the djlib-managed Mixxx crates in line with `_djlib/crates.json`.
///
/// Only crates listed in `djlib_crates` (DjlibStore) are ever created,
/// renamed, locked, deleted or have their tracks changed; every other crate
/// belongs to the DJ and is never read for membership or written.
///
/// - Name clash with a DJ crate: "<name> (djlib)", "<name> (djlib 2)", ...
/// - Renames that would collide with another managed crate (swaps, rotations)
///   go through a temporary name.
/// - Managed crates are locked; a DJ who unlocks/renames one gets name and lock
///   re-asserted on the next reconcile. A DJ who unlocks and deletes one: the
///   mapping is dropped (crateDeleted) and the next reconcile recreates the
///   crate (empty; the caller re-applies membership for the ids reported in
///   `pCreatedIds`).
///
/// Main thread only.
class DjlibCrateSync : public QObject {
    Q_OBJECT
  public:
    DjlibCrateSync(TrackCollection* pTrackCollection,
            DjlibStore* pStore,
            QObject* pParent = nullptr);
    ~DjlibCrateSync() override = default;

    /// Brings the managed crates in line with `crates` (the parsed crates.json):
    /// deletes managed crates no longer listed (their tracks stay in the
    /// library), renames and re-locks the listed ones, creates missing ones.
    /// Returns false if any step failed (the rest is still attempted).
    /// `pCreatedIds` receives the djlib ids of newly created crates.
    bool reconcile(const QList<djlib::CrateRef>& crates,
            QStringList* pCreatedIds = nullptr);

    /// Sets the track's membership in managed crates to exactly the given djlib
    /// crate ids. Ids without a managed crate (i.e. not in crates.json) are
    /// ignored, which implements "sidecar crates ∩ crates.json". Non-managed
    /// crates are never touched.
    bool setMembership(TrackId trackId, const QStringList& djlibCrateIds);

  private slots:
    void slotCrateDeleted(CrateId crateId);

  private:
    QString temporaryName(const QString& djlibId) const;

    TrackCollection* const m_pTrackCollection;
    DjlibStore* const m_pStore;
};
