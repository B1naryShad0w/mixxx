#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QStringList>

#include "library/djlib/djlibcratesync.h"
#include "library/djlib/djlibstore.h"
#include "library/trackset/crate/crate.h"
#include "library/trackset/crate/cratetablemodel.h"
#include "test/librarytest.h"
#include "track/track.h"

using djlib::CrateRef;

namespace {

const QString kTrack1 = QStringLiteral("id3-test-data/artist.mp3");
const QString kTrack2 = QStringLiteral("id3-test-data/TOAL_TPE2.mp3");

} // namespace

class DjlibStoreTest : public LibraryTest {
  protected:
    DjlibStoreTest()
            : m_store(internalCollection()->database()) {
        EXPECT_TRUE(m_store.initialize());
    }

    DjlibStore m_store;
};

TEST_F(DjlibStoreTest, initializeIsIdempotent) {
    EXPECT_TRUE(m_store.initialize());
    DjlibStore other(internalCollection()->database());
    EXPECT_TRUE(other.initialize());
    QSqlQuery query(internalCollection()->database());
    ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name IN ('djlib_crates','djlib_tracks') ORDER BY name")));
    QStringList names;
    while (query.next()) {
        names << query.value(0).toString();
    }
    EXPECT_EQ(QStringList({"djlib_crates", "djlib_tracks"}), names);
}

TEST_F(DjlibStoreTest, crateMappingCrud) {
    EXPECT_TRUE(m_store.crateMappings().isEmpty());
    const DjlibStore::CrateMapping mapping{
            QStringLiteral("crate_a"), CrateId(QVariant(7)), QStringLiteral("House")};
    ASSERT_TRUE(m_store.upsertCrateMapping(mapping));

    auto byId = m_store.crateMapping(QStringLiteral("crate_a"));
    ASSERT_TRUE(byId.has_value());
    EXPECT_EQ(CrateId(QVariant(7)), byId->crateId);
    EXPECT_EQ(QStringLiteral("House"), byId->name);
    auto byCrate = m_store.crateMappingByCrateId(CrateId(QVariant(7)));
    ASSERT_TRUE(byCrate.has_value());
    EXPECT_EQ(QStringLiteral("crate_a"), byCrate->djlibId);

    // Update in place
    ASSERT_TRUE(m_store.upsertCrateMapping(
            {QStringLiteral("crate_a"), CrateId(QVariant(7)), QStringLiteral("Techno")}));
    EXPECT_EQ(1, m_store.crateMappings().size());
    EXPECT_EQ(QStringLiteral("Techno"), m_store.crateMapping(QStringLiteral("crate_a"))->name);

    ASSERT_TRUE(m_store.removeCrateMapping(QStringLiteral("crate_a")));
    EXPECT_FALSE(m_store.crateMapping(QStringLiteral("crate_a")).has_value());
    EXPECT_FALSE(m_store.crateMappingByCrateId(CrateId(QVariant(7))).has_value());
}

TEST_F(DjlibStoreTest, trackStateCrud) {
    DjlibStore::TrackState state;
    state.djlibId = QStringLiteral("trk_1");
    state.trackId = TrackId(QVariant(42));
    state.file = QStringLiteral("A - B [1].stem.mp4");
    state.sidecarVersion = 3;
    state.packageVersion = 1;
    state.appliedBeats = QStringLiteral("beats-fp");
    state.appliedCues = QStringLiteral("cues-fp");
    state.appliedKey = 21;
    ASSERT_TRUE(m_store.upsertTrackState(state));

    auto loaded = m_store.trackState(QStringLiteral("trk_1"));
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(TrackId(QVariant(42)), loaded->trackId);
    EXPECT_EQ(state.file, loaded->file);
    EXPECT_EQ(3, loaded->sidecarVersion);
    EXPECT_EQ(std::optional<qint64>(1), loaded->packageVersion);
    EXPECT_EQ(QStringLiteral("beats-fp"), loaded->appliedBeats);
    EXPECT_EQ(QStringLiteral("cues-fp"), loaded->appliedCues);
    EXPECT_EQ(std::optional<int>(21), loaded->appliedKey);
    EXPECT_FALSE(loaded->pendingJson.has_value());
    EXPECT_GT(loaded->updatedAt, 0);

    auto byTrack = m_store.trackStateByTrackId(TrackId(QVariant(42)));
    ASSERT_TRUE(byTrack.has_value());
    EXPECT_EQ(QStringLiteral("trk_1"), byTrack->djlibId);

    // Pending sidecar, no track yet
    DjlibStore::TrackState pending;
    pending.djlibId = QStringLiteral("trk_2");
    pending.file = QStringLiteral("C - D [2].stem.mp4");
    pending.sidecarVersion = 0;
    ASSERT_TRUE(m_store.upsertTrackState(pending));
    ASSERT_TRUE(m_store.setPendingJson(QStringLiteral("trk_2"), QByteArrayLiteral("{\"x\":1}")));
    loaded = m_store.trackState(QStringLiteral("trk_2"));
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->trackId.isValid());
    EXPECT_FALSE(loaded->packageVersion.has_value());
    EXPECT_FALSE(loaded->appliedKey.has_value());
    EXPECT_EQ(std::optional<QByteArray>(QByteArrayLiteral("{\"x\":1}")), loaded->pendingJson);
    EXPECT_EQ(2, m_store.trackStates().size());

    ASSERT_TRUE(m_store.setPendingJson(QStringLiteral("trk_2"), std::nullopt));
    EXPECT_FALSE(m_store.trackState(QStringLiteral("trk_2"))->pendingJson.has_value());

    ASSERT_TRUE(m_store.removeTrackStatesByTrackIds({TrackId(QVariant(42))}));
    EXPECT_FALSE(m_store.trackState(QStringLiteral("trk_1")).has_value());
    ASSERT_TRUE(m_store.removeTrackState(QStringLiteral("trk_2")));
    EXPECT_TRUE(m_store.trackStates().isEmpty());
}

class DjlibCrateSyncTest : public LibraryTest {
  protected:
    DjlibCrateSyncTest()
            : m_store(internalCollection()->database()),
              m_sync(internalCollection(), &m_store) {
        EXPECT_TRUE(m_store.initialize());
    }

    CrateId managedCrateId(const QString& djlibId) const {
        const auto mapping = m_store.crateMapping(djlibId);
        return mapping ? mapping->crateId : CrateId();
    }

    Crate readCrate(CrateId id) const {
        Crate crate;
        EXPECT_TRUE(internalCollection()->crates().readCrateById(id, &crate));
        return crate;
    }

    CrateId insertDjCrate(const QString& name) {
        Crate crate;
        crate.setName(name);
        CrateId id;
        EXPECT_TRUE(internalCollection()->insertCrate(crate, &id));
        return id;
    }

    QList<CrateId> cratesOfTrack(TrackId trackId) const {
        QList<CrateId> result;
        auto select = internalCollection()->crates().selectTrackCratesSorted(trackId);
        while (select.next()) {
            result.append(select.crateId());
        }
        return result;
    }

    TrackId addTrack(const QString& relPath) const {
        const auto pTrack = getOrAddTrackByLocation(getTestDir().filePath(relPath));
        EXPECT_TRUE(pTrack);
        return pTrack ? pTrack->getId() : TrackId();
    }

    DjlibStore m_store;
    DjlibCrateSync m_sync;
};

TEST_F(DjlibCrateSyncTest, createsLockedCrates) {
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}, {"crate_b", "Requests / Halloween 2026"}}));
    EXPECT_EQ(2u, internalCollection()->crates().countCrates());
    const Crate a = readCrate(managedCrateId("crate_a"));
    EXPECT_EQ(QStringLiteral("House"), a.getName());
    EXPECT_TRUE(a.isLocked());
    const Crate b = readCrate(managedCrateId("crate_b"));
    EXPECT_EQ(QStringLiteral("Requests / Halloween 2026"), b.getName());
    EXPECT_TRUE(b.isLocked());

    // Idempotent
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}, {"crate_b", "Requests / Halloween 2026"}}));
    EXPECT_EQ(2u, internalCollection()->crates().countCrates());
    EXPECT_EQ(a.getId(), managedCrateId("crate_a"));
}

TEST_F(DjlibCrateSyncTest, renameKeepsCrateAndMembership) {
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}}));
    const CrateId id = managedCrateId("crate_a");
    const TrackId track = addTrack(kTrack1);
    ASSERT_TRUE(m_sync.setMembership(track, {"crate_a"}));

    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "Deep House"}}));
    EXPECT_EQ(id, managedCrateId("crate_a"));
    EXPECT_EQ(QStringLiteral("Deep House"), readCrate(id).getName());
    EXPECT_EQ(QStringLiteral("Deep House"), m_store.crateMapping("crate_a")->name);
    EXPECT_EQ(QList<CrateId>({id}), cratesOfTrack(track));
}

TEST_F(DjlibCrateSyncTest, swapNamesUsesTemporaryName) {
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}, {"crate_b", "Techno"}}));
    const CrateId a = managedCrateId("crate_a");
    const CrateId b = managedCrateId("crate_b");

    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "Techno"}, {"crate_b", "House"}}));
    EXPECT_EQ(a, managedCrateId("crate_a"));
    EXPECT_EQ(b, managedCrateId("crate_b"));
    EXPECT_EQ(QStringLiteral("Techno"), readCrate(a).getName());
    EXPECT_EQ(QStringLiteral("House"), readCrate(b).getName());
    EXPECT_EQ(2u, internalCollection()->crates().countCrates());

    // Three-way rotation
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "X"}, {"crate_b", "Y"}, {"crate_c", "Z"}}));
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "Y"}, {"crate_b", "Z"}, {"crate_c", "X"}}));
    EXPECT_EQ(QStringLiteral("Y"), readCrate(managedCrateId("crate_a")).getName());
    EXPECT_EQ(QStringLiteral("Z"), readCrate(managedCrateId("crate_b")).getName());
    EXPECT_EQ(QStringLiteral("X"), readCrate(managedCrateId("crate_c")).getName());
    EXPECT_EQ(3u, internalCollection()->crates().countCrates());
}

TEST_F(DjlibCrateSyncTest, clashWithDjCrateGetsSuffix) {
    const CrateId dj1 = insertDjCrate(QStringLiteral("House"));
    const CrateId dj2 = insertDjCrate(QStringLiteral("Techno (djlib)"));
    const TrackId track = addTrack(kTrack1);
    ASSERT_TRUE(internalCollection()->addCrateTracks(dj1, {track}));

    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}, {"crate_b", "Techno"}, {"crate_c", "Techno (djlib)"}}));
    EXPECT_EQ(QStringLiteral("House (djlib)"), readCrate(managedCrateId("crate_a")).getName());
    // "Techno" itself is free
    EXPECT_EQ(QStringLiteral("Techno"), readCrate(managedCrateId("crate_b")).getName());
    EXPECT_EQ(QStringLiteral("Techno (djlib) (djlib)"),
            readCrate(managedCrateId("crate_c")).getName());

    // Second clash level
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}, {"crate_b", "Techno"},
            {"crate_c", "Techno (djlib)"}, {"crate_d", "House"}}));
    EXPECT_EQ(QStringLiteral("House (djlib)"), readCrate(managedCrateId("crate_a")).getName());
    EXPECT_EQ(QStringLiteral("House (djlib 2)"), readCrate(managedCrateId("crate_d")).getName());

    // DJ crates untouched
    const Crate djCrate1 = readCrate(dj1);
    EXPECT_EQ(QStringLiteral("House"), djCrate1.getName());
    EXPECT_FALSE(djCrate1.isLocked());
    EXPECT_EQ(QStringLiteral("Techno (djlib)"), readCrate(dj2).getName());
    EXPECT_EQ(QList<CrateId>({dj1}), cratesOfTrack(track));
    EXPECT_FALSE(m_store.crateMappingByCrateId(dj1).has_value());

    // DJ renames their crate away: the managed crate takes the plain name
    Crate renamed = djCrate1;
    renamed.setName(QStringLiteral("My House"));
    ASSERT_TRUE(internalCollection()->updateCrate(renamed));
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}}));
    EXPECT_EQ(QStringLiteral("House"), readCrate(managedCrateId("crate_a")).getName());
}

TEST_F(DjlibCrateSyncTest, removedCrateIsDeletedTrackKept) {
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}, {"crate_b", "Techno"}}));
    const CrateId a = managedCrateId("crate_a");
    const TrackId track = addTrack(kTrack1);
    ASSERT_TRUE(m_sync.setMembership(track, {"crate_a", "crate_b"}));

    ASSERT_TRUE(m_sync.reconcile({{"crate_b", "Techno"}}));
    EXPECT_FALSE(internalCollection()->crates().readCrateById(a));
    EXPECT_FALSE(m_store.crateMapping("crate_a").has_value());
    EXPECT_EQ(1u, internalCollection()->crates().countCrates());
    EXPECT_EQ(QList<CrateId>({managedCrateId("crate_b")}), cratesOfTrack(track));
    EXPECT_TRUE(trackCollectionManager()->getTrackById(track));

    // Removed and re-added in one go: a crate whose name was freed by a delete
    ASSERT_TRUE(m_sync.reconcile({{"crate_c", "Techno"}}));
    EXPECT_EQ(QStringLiteral("Techno"), readCrate(managedCrateId("crate_c")).getName());
    EXPECT_EQ(1u, internalCollection()->crates().countCrates());
}

TEST_F(DjlibCrateSyncTest, djDeletedManagedCrateIsRecreated) {
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}}));
    const CrateId oldId = managedCrateId("crate_a");

    // The DJ unlocks and deletes the crate
    Crate crate = readCrate(oldId);
    crate.setLocked(false);
    ASSERT_TRUE(internalCollection()->updateCrate(crate));
    ASSERT_TRUE(internalCollection()->deleteCrate(oldId));
    EXPECT_FALSE(m_store.crateMapping("crate_a").has_value());

    QStringList created;
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}}, &created));
    EXPECT_EQ(QStringList({"crate_a"}), created);
    const CrateId newId = managedCrateId("crate_a");
    ASSERT_TRUE(newId.isValid());
    const Crate recreated = readCrate(newId);
    EXPECT_EQ(QStringLiteral("House"), recreated.getName());
    EXPECT_TRUE(recreated.isLocked());
}

TEST_F(DjlibCrateSyncTest, mappingToVanishedCrateIsDropped) {
    // E.g. the crate was deleted while no DjlibCrateSync was listening
    ASSERT_TRUE(m_store.upsertCrateMapping({"crate_a", CrateId(QVariant(999)), "House"}));
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}}));
    const CrateId id = managedCrateId("crate_a");
    ASSERT_TRUE(id.isValid());
    EXPECT_NE(CrateId(QVariant(999)), id);
    EXPECT_EQ(QStringLiteral("House"), readCrate(id).getName());
}

TEST_F(DjlibCrateSyncTest, djUnlockAndRenameIsReasserted) {
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}}));
    const CrateId id = managedCrateId("crate_a");
    Crate crate = readCrate(id);
    crate.setLocked(false);
    crate.setName(QStringLiteral("Whatever"));
    ASSERT_TRUE(internalCollection()->updateCrate(crate));

    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}}));
    const Crate after = readCrate(id);
    EXPECT_EQ(QStringLiteral("House"), after.getName());
    EXPECT_TRUE(after.isLocked());
}

TEST_F(DjlibCrateSyncTest, membershipNeverTouchesDjCrates) {
    const CrateId dj = insertDjCrate(QStringLiteral("Mine"));
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}, {"crate_b", "Techno"}}));
    const CrateId a = managedCrateId("crate_a");
    const CrateId b = managedCrateId("crate_b");
    const TrackId t1 = addTrack(kTrack1);
    const TrackId t2 = addTrack(kTrack2);
    ASSERT_TRUE(internalCollection()->addCrateTracks(dj, {t1, t2}));

    // Unknown ids (not in crates.json) are ignored
    ASSERT_TRUE(m_sync.setMembership(t1, {"crate_a", "crate_unknown"}));
    QList<CrateId> expected{dj, a};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(expected, cratesOfTrack(t1));

    ASSERT_TRUE(m_sync.setMembership(t1, {"crate_b"}));
    expected = {dj, b};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(expected, cratesOfTrack(t1));

    ASSERT_TRUE(m_sync.setMembership(t1, {}));
    EXPECT_EQ(QList<CrateId>({dj}), cratesOfTrack(t1));
    // The other track and the DJ crate are untouched
    EXPECT_EQ(QList<CrateId>({dj}), cratesOfTrack(t2));
    EXPECT_EQ(2u, internalCollection()->crates().countCrateTracks(dj));
    EXPECT_FALSE(readCrate(dj).isLocked());
}

// The crate that is open in the track list reloads when the service changes
// its tracks; before, the list only caught up after leaving and re-entering.
TEST_F(DjlibCrateSyncTest, openCrateListFollowsMembershipChanges) {
    ASSERT_TRUE(m_sync.reconcile({{"crate_a", "House"}, {"crate_b", "Techno"}}));
    const TrackId t1 = addTrack(kTrack1);
    const TrackId t2 = addTrack(kTrack2);

    CrateTableModel model(nullptr, trackCollectionManager());
    model.selectCrate(managedCrateId("crate_a"));
    model.select();
    ASSERT_EQ(0, model.rowCount());

    ASSERT_TRUE(m_sync.setMembership(t1, {"crate_a"}));
    ASSERT_TRUE(m_sync.setMembership(t2, {"crate_a"}));
    QCoreApplication::processEvents();
    EXPECT_EQ(2, model.rowCount());

    // A change to another crate leaves the open list alone
    ASSERT_TRUE(m_sync.setMembership(t2, {"crate_b"}));
    QCoreApplication::processEvents();
    EXPECT_EQ(1, model.rowCount());

    ASSERT_TRUE(m_sync.setMembership(t1, {}));
    QCoreApplication::processEvents();
    EXPECT_EQ(0, model.rowCount());
}
