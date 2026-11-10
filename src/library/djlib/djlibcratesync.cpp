#include "library/djlib/djlibcratesync.h"

#include <QHash>
#include <QSet>

#include "library/djlib/djlibstore.h"
#include "library/trackcollection.h"
#include "library/trackset/crate/crate.h"
#include "moc_djlibcratesync.cpp"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("DjlibCrateSync");

QString suffixedName(const QString& baseName, int attempt) {
    if (attempt == 0) {
        return baseName;
    }
    if (attempt == 1) {
        return QStringLiteral("%1 (djlib)").arg(baseName);
    }
    return QStringLiteral("%1 (djlib %2)").arg(baseName).arg(attempt);
}

} // namespace

DjlibCrateSync::DjlibCrateSync(TrackCollection* pTrackCollection,
        DjlibStore* pStore,
        QObject* pParent)
        : QObject(pParent),
          m_pTrackCollection(pTrackCollection),
          m_pStore(pStore) {
    connect(m_pTrackCollection,
            &TrackCollection::crateDeleted,
            this,
            &DjlibCrateSync::slotCrateDeleted);
}

void DjlibCrateSync::slotCrateDeleted(CrateId crateId) {
    // This catches a DJ who unlocked and deleted a managed crate. The next
    // reconcile recreates it if it is still listed in crates.json. Our own
    // deletes (m_deletingOwnCrate) drop the mapping themselves.
    if (crateId != m_deletingOwnCrate && m_pStore->crateMappingByCrateId(crateId)) {
        kLogger.info() << "Managed crate" << crateId << "was deleted, dropping its mapping";
        m_pStore->removeCrateMappingByCrateId(crateId);
    }
}

QString DjlibCrateSync::temporaryName(const QString& djlibId) const {
    const CrateStorage& crates = m_pTrackCollection->crates();
    const QString base = QStringLiteral("djlib-tmp-%1").arg(djlibId);
    QString name = base;
    for (int i = 2; crates.readCrateByName(name); ++i) {
        name = QStringLiteral("%1-%2").arg(base).arg(i);
    }
    return name;
}

bool DjlibCrateSync::reconcile(
        const QList<djlib::CrateRef>& cratesIn, QStringList* pCreatedIds) {
    bool ok = true;
    const CrateStorage& crateStorage = m_pTrackCollection->crates();

    // Desired crates in crates.json order, unique by id.
    QList<djlib::CrateRef> desired;
    QSet<QString> desiredIds;
    for (const auto& ref : cratesIn) {
        if (ref.id.isEmpty() || desiredIds.contains(ref.id)) {
            continue;
        }
        desiredIds.insert(ref.id);
        desired.append({ref.id, ref.name.isEmpty() ? ref.id : ref.name});
    }

    // Existing managed crates; drop mappings whose crate vanished and delete
    // crates no longer listed.
    QHash<QString, Crate> managed; // djlib id -> crate
    for (const auto& mapping : m_pStore->crateMappings()) {
        Crate crate;
        if (!crateStorage.readCrateById(mapping.crateId, &crate)) {
            kLogger.info() << "Managed crate" << mapping.crateId << "is gone, dropping mapping";
            ok &= m_pStore->removeCrateMapping(mapping.djlibId);
            continue;
        }
        if (!desiredIds.contains(mapping.djlibId)) {
            kLogger.info() << "Deleting managed crate" << crate.getName();
            // Delete first: if that fails the crate stays mapped (and is
            // deleted on the next reconcile) instead of becoming an unmapped,
            // locked "DJ crate".
            m_deletingOwnCrate = crate.getId();
            const bool deleted = m_pTrackCollection->deleteCrate(crate.getId());
            m_deletingOwnCrate = CrateId();
            if (deleted) {
                ok &= m_pStore->removeCrateMapping(mapping.djlibId);
            } else {
                ok = false;
            }
            continue;
        }
        managed.insert(mapping.djlibId, crate);
    }

    // Names held by DJ crates are never available to managed crates.
    QSet<CrateId> managedCrateIds;
    for (const auto& crate : std::as_const(managed)) {
        managedCrateIds.insert(crate.getId());
    }
    QSet<QString> djNames;
    {
        auto select = crateStorage.selectCrates();
        Crate crate;
        while (select.populateNext(&crate)) {
            if (!managedCrateIds.contains(crate.getId())) {
                djNames.insert(crate.getName());
            }
        }
    }

    // Target name per desired crate.
    QHash<QString, QString> targetNames; // djlib id -> Mixxx crate name
    QSet<QString> assigned;
    for (const auto& ref : std::as_const(desired)) {
        QString name;
        for (int attempt = 0;; ++attempt) {
            name = suffixedName(ref.name, attempt);
            if (!djNames.contains(name) && !assigned.contains(name)) {
                break;
            }
        }
        assigned.insert(name);
        targetNames.insert(ref.id, name);
    }

    // Re-lock / rename existing managed crates. A target may currently be
    // held by another managed crate (swap, rotation): rename whatever can go
    // directly; if nothing can, move one crate to a temporary name.
    QHash<QString, QString> currentOwner; // managed crate name -> djlib id
    for (auto it = managed.cbegin(); it != managed.cend(); ++it) {
        currentOwner.insert(it.value().getName(), it.key());
    }
    QStringList pendingRenames;
    for (const auto& ref : std::as_const(desired)) {
        if (!managed.contains(ref.id)) {
            continue;
        }
        Crate& crate = managed[ref.id];
        if (crate.getName() != targetNames.value(ref.id)) {
            pendingRenames.append(ref.id);
        } else if (!crate.isLocked()) {
            crate.setLocked(true);
            ok &= m_pTrackCollection->updateCrate(crate);
        }
    }
    auto renameTo = [&](const QString& djlibId, const QString& newName) {
        Crate& crate = managed[djlibId];
        const QString oldName = crate.getName();
        crate.setName(newName);
        crate.setLocked(true);
        if (!m_pTrackCollection->updateCrate(crate)) {
            crate.setName(oldName);
            return false;
        }
        currentOwner.remove(oldName);
        currentOwner.insert(newName, djlibId);
        return true;
    };
    while (!pendingRenames.isEmpty()) {
        bool progress = false;
        for (auto it = pendingRenames.begin(); it != pendingRenames.end();) {
            const QString target = targetNames.value(*it);
            if (currentOwner.contains(target)) {
                ++it;
                continue;
            }
            kLogger.info() << "Renaming managed crate" << managed[*it].getName() << "to" << target;
            ok &= renameTo(*it, target);
            it = pendingRenames.erase(it);
            progress = true;
        }
        if (!progress) {
            // Cycle: park the first one under a temporary name.
            const QString djlibId = pendingRenames.first();
            if (!renameTo(djlibId, temporaryName(djlibId))) {
                // Leave the remaining renames to the next reconcile; still
                // create missing crates and update mappings below.
                kLogger.warning() << "Failed to move managed crate to a temporary name";
                ok = false;
                break;
            }
        }
    }

    // Update stored djlib names and create missing crates.
    for (const auto& ref : std::as_const(desired)) {
        if (managed.contains(ref.id)) {
            const auto mapping = m_pStore->crateMapping(ref.id);
            if (!mapping || mapping->name != ref.name) {
                ok &= m_pStore->upsertCrateMapping({ref.id, managed[ref.id].getId(), ref.name});
            }
            continue;
        }
        const QString name = targetNames.value(ref.id);
        if (crateStorage.readCrateByName(name)) {
            kLogger.warning() << "Cannot create managed crate, name taken:" << name;
            ok = false;
            continue;
        }
        Crate crate;
        crate.setName(name);
        crate.setLocked(true);
        CrateId crateId;
        if (!m_pTrackCollection->insertCrate(crate, &crateId)) {
            ok = false;
            continue;
        }
        kLogger.info() << "Created managed crate" << name;
        if (!m_pStore->upsertCrateMapping({ref.id, crateId, ref.name})) {
            // Never leave an unmapped locked crate behind (it would look
            // like a DJ crate forever): undo, retried on the next reconcile.
            m_deletingOwnCrate = crateId;
            m_pTrackCollection->deleteCrate(crateId);
            m_deletingOwnCrate = CrateId();
            ok = false;
            continue;
        }
        if (pCreatedIds) {
            pCreatedIds->append(ref.id);
        }
    }
    return ok;
}

bool DjlibCrateSync::setMembership(TrackId trackId, const QStringList& djlibCrateIds) {
    VERIFY_OR_DEBUG_ASSERT(trackId.isValid()) {
        return false;
    }
    QSet<CrateId> managedCrateIds;
    QSet<CrateId> wanted;
    const QSet<QString> wantedDjlibIds(djlibCrateIds.cbegin(), djlibCrateIds.cend());
    for (const auto& mapping : m_pStore->crateMappings()) {
        managedCrateIds.insert(mapping.crateId);
        if (wantedDjlibIds.contains(mapping.djlibId)) {
            wanted.insert(mapping.crateId);
        }
    }

    QSet<CrateId> current; // managed crates only
    {
        auto select = m_pTrackCollection->crates().selectTrackCratesSorted(trackId);
        while (select.next()) {
            const CrateId crateId = select.crateId();
            if (managedCrateIds.contains(crateId)) {
                current.insert(crateId);
            }
        }
    }

    bool ok = true;
    const QList<TrackId> trackIds{trackId};
    for (const auto& crateId : std::as_const(wanted)) {
        if (!current.contains(crateId)) {
            ok &= m_pTrackCollection->addCrateTracks(crateId, trackIds);
        }
    }
    for (const auto& crateId : std::as_const(current)) {
        if (!wanted.contains(crateId)) {
            ok &= m_pTrackCollection->removeCrateTracks(crateId, trackIds);
        }
    }
    return ok;
}
