#include "library/djlib/djlibservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlQuery>
#include <QtConcurrent>
#include <algorithm>

#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "moc_djlibservice.cpp"
#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/fileinfo.h"
#include "util/logger.h"

using djlib::FileStamp;
using djlib::ParsedSidecar;
using djlib::PassResult;
using djlib::SidecarCrates;

const ConfigKey DjlibService::kLibraryPathKey =
        ConfigKey(QStringLiteral("[djlib]"), QStringLiteral("LibraryPath"));

namespace {

const mixxx::Logger kLogger("DjlibService");

const QString kTracksDir = QStringLiteral("tracks");
const QString kDjlibDir = QStringLiteral("_djlib");
const QString kCratesFile = QStringLiteral("crates.json");
const QString kPackageSuffix = QStringLiteral(".stem.mp4");
const QString kSidecarSuffix = QStringLiteral(".djlib.json");

DjlibService* s_pInstance = nullptr;

bool isPackageName(const QString& name) {
    return name.endsWith(kPackageSuffix, Qt::CaseInsensitive);
}

bool isSidecarName(const QString& name) {
    return name.endsWith(kSidecarSuffix, Qt::CaseInsensitive);
}

/// Only packages and sidecars; never temp files of the agent.
bool isWatchedName(const QString& name) {
    return !name.contains(QStringLiteral(".tmp"), Qt::CaseInsensitive) &&
            (isPackageName(name) || isSidecarName(name));
}

QString packageNameOf(const QString& sidecarName) {
    return sidecarName.chopped(kSidecarSuffix.size()) + kPackageSuffix;
}

QString sidecarNameOf(const QString& packageName) {
    return packageName.chopped(kPackageSuffix.size()) + kSidecarSuffix;
}

FileStamp stampOf(const QFileInfo& info) {
    return FileStamp{info.size(), info.lastModified().toMSecsSinceEpoch()};
}

/// Reads a whole small file; the handle is closed before returning.
std::optional<QByteArray> readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

std::optional<DjlibSidecar> readSidecar(const QString& path, QByteArray* pJson) {
    const auto json = readFile(path);
    if (!json) {
        kLogger.warning() << "Cannot read" << path;
        return std::nullopt;
    }
    QString error;
    auto sidecar = DjlibSidecar::parse(*json, &error);
    if (!sidecar) {
        kLogger.warning() << "Invalid sidecar" << path << error;
        return std::nullopt;
    }
    if (pJson) {
        *pJson = *json;
    }
    return sidecar;
}

QHash<QString, FileStamp> listTracksDir(const QString& tracksDir) {
    QHash<QString, FileStamp> result;
    const auto entries = QDir(tracksDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const auto& info : entries) {
        if (isWatchedName(info.fileName())) {
            result.insert(info.fileName(), stampOf(info));
        }
    }
    return result;
}

/// The crates of every sidecar whose package is present (all of them, or only
/// those listing one of `onlyCrateIds`).
QList<SidecarCrates> collectMemberships(const QString& tracksDir,
        const QHash<QString, FileStamp>& snapshot,
        const QSet<QString>& onlyCrateIds) {
    QList<SidecarCrates> result;
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        if (!isSidecarName(it.key()) || !snapshot.contains(packageNameOf(it.key()))) {
            continue;
        }
        const auto sidecar = readSidecar(QDir(tracksDir).filePath(it.key()), nullptr);
        if (!sidecar) {
            continue;
        }
        SidecarCrates entry{sidecar->trackId, {}};
        bool relevant = onlyCrateIds.isEmpty();
        for (const auto& crate : sidecar->crates) {
            entry.crateIds.append(crate.id);
            relevant |= onlyCrateIds.contains(crate.id);
        }
        if (relevant) {
            result.append(entry);
        }
    }
    return result;
}

/// One pass on a worker thread: list, diff against `snapshot`, read and parse
/// what changed. Touches no library state.
PassResult runPass(quint64 generation,
        const QString& tracksDir,
        const QString& cratesPath,
        const QHash<QString, FileStamp>& snapshot,
        const std::optional<FileStamp>& cratesStamp) {
    PassResult result;
    result.generation = generation;
    result.snapshot = listTracksDir(tracksDir);

    const QFileInfo cratesInfo(cratesPath);
    if (cratesInfo.exists()) {
        result.cratesStamp = stampOf(cratesInfo);
    }
    if (result.cratesStamp && result.cratesStamp != cratesStamp) {
        result.cratesChanged = true;
        const auto json = readFile(cratesPath);
        QString error;
        if (json) {
            result.crates = djlib::parseCratesJson(*json, &error);
        }
        if (!result.crates) {
            // Never reconcile on a read/parse failure: that would delete
            // every managed crate. Retried when the file changes again.
            kLogger.warning() << "Invalid" << cratesPath << error;
        }
    }

    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        if (isPackageName(it.key()) && !result.snapshot.contains(it.key())) {
            result.removedPackages.append(it.key());
        }
    }
    QSet<QString> sidecarsToRead;
    for (auto it = result.snapshot.constBegin(); it != result.snapshot.constEnd(); ++it) {
        const auto previous = snapshot.constFind(it.key());
        if (previous != snapshot.constEnd() && *previous == it.value()) {
            continue;
        }
        if (isPackageName(it.key())) {
            result.addedPackages.append(it.key());
            // A sidecar that may have waited for this package.
            const QString sidecarName = sidecarNameOf(it.key());
            if (result.snapshot.contains(sidecarName)) {
                sidecarsToRead.insert(sidecarName);
            }
        } else {
            sidecarsToRead.insert(it.key());
        }
    }
    result.addedPackages.sort();
    QStringList sidecarNames(sidecarsToRead.cbegin(), sidecarsToRead.cend());
    sidecarNames.sort();
    for (const auto& name : std::as_const(sidecarNames)) {
        ParsedSidecar parsed;
        parsed.sidecarName = name;
        auto sidecar = readSidecar(QDir(tracksDir).filePath(name), &parsed.json);
        if (sidecar) {
            parsed.sidecar = std::move(*sidecar);
            result.sidecars.append(std::move(parsed));
        }
    }
    if (result.crates) {
        result.memberships = collectMemberships(tracksDir, result.snapshot, {});
    }
    return result;
}

} // namespace

DjlibService::DjlibService(UserSettingsPointer pConfig,
        TrackCollectionManager* pTrackCollectionManager,
        QObject* pParent,
        Timing timing)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_timing(timing),
          m_store(pTrackCollectionManager->internalCollection()->database()),
          m_crateSync(pTrackCollectionManager->internalCollection(), &m_store),
          m_importer(&m_crateSync) {
    if (!m_store.initialize()) {
        kLogger.warning() << "Cannot create the djlib tables";
    }
    // Workers: one pass/membership job plus one sample-rate probe at a time.
    m_pool.setMaxThreadCount(2);

    m_debounceTimer.setSingleShot(true);
    connect(&m_debounceTimer, &QTimer::timeout, this, &DjlibService::startPass);
    m_safetyTimer.setInterval(m_timing.safetyPassMillis);
    connect(&m_safetyTimer, &QTimer::timeout, this, &DjlibService::requestPass);
    // One queue item per event-loop turn.
    m_queueTimer.setSingleShot(true);
    m_queueTimer.setInterval(0);
    connect(&m_queueTimer, &QTimer::timeout, this, &DjlibService::processNext);
    connect(&m_watcher,
            &QFileSystemWatcher::directoryChanged,
            this,
            &DjlibService::slotDirectoryChanged);

    TrackCollection* pCollection = m_pTrackCollectionManager->internalCollection();
    // Purged tracks: drop their state. tracksRemoved is also emitted for
    // hidden and relocated tracks, so only rows gone from `library` count.
    connect(pCollection,
            &TrackCollection::tracksRemoved,
            this,
            [this, pCollection](const QSet<TrackId>& trackIds) {
                QList<TrackId> purged;
                QSqlQuery query(pCollection->database());
                query.prepare(QStringLiteral("SELECT 1 FROM library WHERE id = :id"));
                for (const auto& trackId : trackIds) {
                    if (!m_store.trackStateByTrackId(trackId)) {
                        continue;
                    }
                    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
                    if (query.exec() && !query.next()) {
                        purged.append(trackId);
                    }
                }
                if (!purged.isEmpty()) {
                    m_store.removeTrackStatesByTrackIds(purged);
                    for (auto it = m_deferred.begin(); it != m_deferred.end();) {
                        it = purged.contains(it->trackId) ? m_deferred.erase(it) : ++it;
                    }
                    emit statusChanged();
                }
            });
    // A DJ who unlocks and deletes a managed crate: DjlibCrateSync drops the
    // mapping, the next reconcile recreates it (and its members).
    connect(pCollection,
            &TrackCollection::crateDeleted,
            this,
            &DjlibService::scheduleReconcile);

    s_pInstance = this;
}

DjlibService::~DjlibService() {
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
    m_generation++;
    m_pool.waitForDone();
}

// static
DjlibService* DjlibService::instance() {
    return s_pInstance;
}

void DjlibService::setTrackLoadedFn(TrackLoadedFn fn) {
    m_trackLoadedFn = std::move(fn);
}

void DjlibService::start() {
    m_libraryPath = m_pConfig->getValueString(kLibraryPathKey).trimmed();
    rearm();
}

void DjlibService::setLibraryPath(const QString& libraryPath) {
    const QString path = libraryPath.trimmed();
    m_pConfig->setValue(kLibraryPathKey, path);
    if (path == m_libraryPath && (path.isEmpty() || m_safetyTimer.isActive())) {
        return;
    }
    m_libraryPath = path;
    rearm();
}

QString DjlibService::tracksDir() const {
    return QDir(m_libraryPath).filePath(kTracksDir);
}

QString DjlibService::djlibDir() const {
    return QDir(m_libraryPath).filePath(kDjlibDir);
}

void DjlibService::rearm() {
    // Results of jobs still running for the old path are discarded.
    m_generation++;
    m_debounceTimer.stop();
    m_safetyTimer.stop();
    m_queueTimer.stop();
    m_burst.invalidate();
    if (!m_watcher.directories().isEmpty()) {
        m_watcher.removePaths(m_watcher.directories());
    }
    m_snapshot.clear();
    m_cratesStamp.reset();
    m_crates.reset();
    m_queue.clear();
    m_deferred.clear();
    m_passRequested = false;
    m_reconcileQueued = false;

    if (m_libraryPath.isEmpty()) {
        kLogger.info() << "Disabled (no library path)";
        emit statusChanged();
        return;
    }
    kLogger.info() << "Watching" << m_libraryPath;
    m_safetyTimer.start();
    startPass(); // full pass: the snapshot is empty
    emit statusChanged();
}

void DjlibService::registerLibraryRoot() {
    const QString dir = tracksDir();
    if (m_registeredRoot == dir || !QFileInfo(dir).isDir()) {
        return;
    }
    // Once per path: stock rescans, missing-track handling and Browse then
    // cover the djlib tracks too.
    m_registeredRoot = dir;
    const auto result = m_pTrackCollectionManager->addDirectory(mixxx::FileInfo(dir));
    if (result != DirectoryDAO::AddResult::Ok &&
            result != DirectoryDAO::AddResult::AlreadyWatching) {
        kLogger.warning() << "Cannot add library root" << dir << static_cast<int>(result);
    }
}

void DjlibService::watchDirectories() {
    const QStringList watched = m_watcher.directories();
    for (const auto& dir : {tracksDir(), djlibDir()}) {
        if (!watched.contains(dir) && QFileInfo(dir).isDir()) {
            m_watcher.addPath(dir);
        }
    }
}

void DjlibService::slotDirectoryChanged() {
    // Debounce, but force a pass after maxDelayMillis of continuous events.
    if (!m_burst.isValid()) {
        m_burst.start();
    }
    const qint64 remaining = m_timing.maxDelayMillis - m_burst.elapsed();
    m_debounceTimer.start(static_cast<int>(
            std::clamp<qint64>(remaining, 0, m_timing.debounceMillis)));
}

void DjlibService::requestPass() {
    startPass();
}

void DjlibService::startPass() {
    if (m_libraryPath.isEmpty()) {
        return;
    }
    m_debounceTimer.stop();
    m_burst.invalidate();
    if (m_passRunning) {
        m_passRequested = true;
        return;
    }
    // Directories may only appear with the first sync.
    registerLibraryRoot();
    watchDirectories();
    m_passRunning = true;
    QtConcurrent::run(&m_pool,
            runPass,
            m_generation,
            tracksDir(),
            QDir(djlibDir()).filePath(kCratesFile),
            m_snapshot,
            m_cratesStamp)
            .then(this, [this](PassResult result) {
                onPassFinished(std::move(result));
            });
}

void DjlibService::onPassFinished(PassResult result) {
    m_passRunning = false;
    if (result.generation == m_generation) {
        m_snapshot = std::move(result.snapshot);
        if (result.cratesChanged) {
            // An invalid file is retried only once it changes again.
            m_cratesStamp = result.cratesStamp;
        }
        // crates.json first: memberships need the managed crates.
        if (result.crates) {
            enqueue(Item{Item::Kind::Reconcile, {}, {}, *result.crates, {}, true});
        }
        for (const auto& name : std::as_const(result.removedPackages)) {
            enqueue(Item{Item::Kind::PackageRemoved, name, {}, {}, {}, false});
        }
        for (const auto& name : std::as_const(result.addedPackages)) {
            enqueue(Item{Item::Kind::PackageAdded, name, {}, {}, {}, false});
        }
        for (auto& parsed : result.sidecars) {
            const QString name = parsed.sidecarName;
            enqueue(Item{Item::Kind::Sidecar, name, std::move(parsed), {}, {}, false});
        }
        for (const auto& membership : std::as_const(result.memberships)) {
            enqueue(Item{Item::Kind::Membership,
                    membership.djlibId,
                    {},
                    {},
                    membership.crateIds,
                    false});
        }
    }
    if (m_passRequested) {
        m_passRequested = false;
        startPass();
    }
    emit statusChanged();
    checkIdle();
}

void DjlibService::startMembershipJob(const QSet<QString>& crateIds) {
    if (m_libraryPath.isEmpty() || crateIds.isEmpty()) {
        return;
    }
    ++m_jobsRunning;
    const quint64 generation = m_generation;
    QtConcurrent::run(&m_pool, collectMemberships, tracksDir(), m_snapshot, crateIds)
            .then(this, [this, generation](QList<SidecarCrates> memberships) {
                --m_jobsRunning;
                if (generation == m_generation) {
                    for (const auto& membership : std::as_const(memberships)) {
                        enqueue(Item{Item::Kind::Membership,
                                membership.djlibId,
                                {},
                                {},
                                membership.crateIds,
                                false});
                    }
                }
                checkIdle();
            });
}

void DjlibService::startProbe(const TrackPointer& pTrack) {
    const TrackId trackId = pTrack->getId();
    if (!trackId.isValid() || m_probing.contains(trackId) || m_probed.contains(trackId)) {
        return;
    }
    m_probing.insert(trackId);
    ++m_jobsRunning;
    // Opens the audio source once to read the stream info (sample rate), as
    // AnalyzerThread does. The source is closed again right away.
    QtConcurrent::run(&m_pool,
            [pTrack] {
                SoundSourceProxy(pTrack).openAudioSource();
            })
            .then(this, [this, trackId] {
                --m_jobsRunning;
                m_probing.remove(trackId);
                m_probed.insert(trackId);
                for (auto it = m_deferred.constBegin(); it != m_deferred.constEnd(); ++it) {
                    if (it->trackId == trackId) {
                        enqueue(Item{Item::Kind::Retry, it.key(), {}, {}, {}, false});
                    }
                }
                checkIdle();
            });
}

void DjlibService::scheduleReconcile() {
    if (m_reconcileQueued || !m_crates) {
        return;
    }
    m_reconcileQueued = true;
    enqueue(Item{Item::Kind::Reconcile, {}, {}, *m_crates, {}, false});
}

void DjlibService::enqueue(Item item) {
    m_queue.enqueue(std::move(item));
    if (!m_queueTimer.isActive()) {
        m_queueTimer.start();
    }
}

void DjlibService::processNext() {
    if (!m_queue.isEmpty()) {
        const Item item = m_queue.dequeue();
        process(item);
    }
    if (!m_queue.isEmpty()) {
        m_queueTimer.start(); // next item in the next event-loop turn
    } else {
        emit statusChanged();
        checkIdle();
    }
}

void DjlibService::checkIdle() {
    if (m_queue.isEmpty() && !m_passRunning && m_jobsRunning == 0) {
        emit idle();
    }
}

void DjlibService::process(const Item& item) {
    switch (item.kind) {
    case Item::Kind::Reconcile:
        reconcile(item.crates, item.membershipsIncluded);
        break;
    case Item::Kind::PackageRemoved:
        packageRemoved(item.name);
        break;
    case Item::Kind::PackageAdded:
        packageAdded(item.name);
        break;
    case Item::Kind::Sidecar:
        sidecarChanged(item.sidecar);
        break;
    case Item::Kind::Membership:
        applyMembership(item.name, item.crateIds);
        break;
    case Item::Kind::Retry:
        retry(item.name);
        break;
    }
}

void DjlibService::reconcile(const QList<djlib::CrateRef>& crates, bool membershipsIncluded) {
    m_reconcileQueued = false;
    m_crates = crates;
    QStringList createdIds;
    if (!m_crateSync.reconcile(crates, &createdIds)) {
        kLogger.warning() << "crates.json reconcile had errors";
    }
    // Recreated crates start empty. After a crates.json change the membership
    // of all tracks is re-applied anyway (queued behind this item).
    if (!membershipsIncluded && !createdIds.isEmpty()) {
        startMembershipJob(QSet<QString>(createdIds.cbegin(), createdIds.cend()));
    }
}

QSet<QString> DjlibService::allowedCrateIds() const {
    QSet<QString> result;
    for (const auto& mapping : m_store.crateMappings()) {
        result.insert(mapping.djlibId);
    }
    return result;
}

bool DjlibService::isTrackLoaded(const TrackPointer& pTrack) const {
    return m_trackLoadedFn && m_trackLoadedFn(pTrack);
}

void DjlibService::packageRemoved(const QString& packageName) {
    if (m_snapshot.contains(packageName)) {
        return; // back again meanwhile
    }
    // The track stays in the library (playlists, history) for Mixxx's
    // missing-track handling; only the djlib crates change.
    for (auto state : m_store.trackStates()) {
        if (state.file != packageName) {
            continue;
        }
        if (state.trackId.isValid()) {
            m_crateSync.setMembership(state.trackId, {});
        }
        m_deferred.remove(state.djlibId);
        // If the package comes back, its sidecar is applied again (the
        // fingerprints stay, so DJ edits still win).
        state.sidecarVersion = 0;
        m_store.upsertTrackState(state);
    }
}

void DjlibService::packageAdded(const QString& packageName) {
    if (!m_snapshot.contains(packageName)) {
        return; // gone again meanwhile
    }
    // Import it so it shows in the library; its sidecar follows.
    const QString path = QDir(tracksDir()).filePath(packageName);
    const TrackPointer pTrack =
            m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(path));
    if (!pTrack) {
        kLogger.warning() << "Cannot add" << path;
        return;
    }
    if (!pTrack->getSampleRate().isValid() && !isTrackLoaded(pTrack)) {
        startProbe(pTrack);
    }
}

void DjlibService::sidecarChanged(const ParsedSidecar& parsed) {
    const DjlibSidecar& sidecar = parsed.sidecar;
    QString packageName = packageNameOf(parsed.sidecarName);
    if (!m_snapshot.contains(packageName) && m_snapshot.contains(sidecar.file)) {
        packageName = sidecar.file;
    }
    const auto existing = m_store.trackState(sidecar.trackId);
    if (!m_snapshot.contains(packageName)) {
        // Sidecar without package: wait for the package.
        if (existing) {
            m_store.setPendingJson(sidecar.trackId, parsed.json);
        } else {
            DjlibStore::TrackState state;
            state.djlibId = sidecar.trackId;
            state.file = sidecar.file;
            state.pendingJson = parsed.json;
            m_store.upsertTrackState(state);
        }
        return;
    }
    // Fast path (startup pass): nothing newer to apply, no library access.
    if (existing && existing->trackId.isValid() && !existing->pendingJson &&
            !m_deferred.contains(sidecar.trackId) &&
            sidecar.sidecarVersion <= existing->sidecarVersion) {
        return;
    }
    const QString path = QDir(tracksDir()).filePath(packageName);
    const TrackPointer pTrack =
            m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(path));
    if (!pTrack) {
        kLogger.warning() << "Cannot add" << path;
        return;
    }
    if (existing && existing->trackId.isValid() && existing->trackId != pTrack->getId()) {
        // The djlib track is another library track now (file renamed while
        // Mixxx was closed; no TrackDAO relocation, see header): start fresh.
        // The old fingerprints describe the old track and would read as DJ
        // edits here.
        kLogger.info() << sidecar.trackId << "moved to" << path << "(new library track)";
        DjlibStore::TrackState state = *existing;
        state.trackId = TrackId();
        state.sidecarVersion = 0;
        state.appliedBeats.clear();
        state.appliedCues.clear();
        state.appliedKey.reset();
        m_store.upsertTrackState(state);
    }
    applyToTrack(pTrack, sidecar);
}

void DjlibService::applyMembership(const QString& djlibId, const QStringList& crateIds) {
    const auto state = m_store.trackState(djlibId);
    if (!state || !state->trackId.isValid()) {
        return;
    }
    // setMembership ignores ids without a managed crate: sidecar ∩ crates.json.
    m_crateSync.setMembership(state->trackId, crateIds);
}

void DjlibService::retry(const QString& djlibId) {
    const auto it = m_deferred.constFind(djlibId);
    if (it == m_deferred.constEnd()) {
        return;
    }
    const Deferred deferred = *it;
    const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(deferred.trackId);
    if (!pTrack) {
        m_deferred.remove(djlibId);
        return;
    }
    applyToTrack(pTrack, deferred.sidecar);
}

void DjlibService::applyToTrack(const TrackPointer& pTrack, const DjlibSidecar& sidecar) {
    const bool loaded = isTrackLoaded(pTrack);
    const auto result = m_importer.apply(pTrack, sidecar, m_store, loaded, allowedCrateIds());
    if (m_pTrackCollectionManager->saveTrack(pTrack) ==
            TrackCollectionManager::SaveTrackResult::Failed) {
        kLogger.warning() << "Saving failed for" << sidecar.trackId;
    }
    if (result.outdated) {
        m_deferred.remove(sidecar.trackId);
        return;
    }
    const auto state = m_store.trackState(sidecar.trackId);
    if (state && state->pendingJson) {
        m_store.setPendingJson(sidecar.trackId, std::nullopt);
    }
    if (result.hasDeferred()) {
        // Completed on deck unload (slotDeckTrackChanged) or after the probe.
        m_deferred.insert(sidecar.trackId, Deferred{sidecar, pTrack->getId()});
        if (result.needsSampleRate && !loaded) {
            if (m_probed.contains(pTrack->getId())) {
                kLogger.warning() << "Sample rate unknown for" << sidecar.trackId
                                  << "- beats/cues wait for a deck load";
            } else {
                startProbe(pTrack);
            }
        }
    } else {
        m_deferred.remove(sidecar.trackId);
        m_lastImport = QDateTime::currentDateTime();
        m_lastImportFile = sidecar.file;
        emit sidecarApplied(sidecar.trackId, sidecar.sidecarVersion);
    }
}

void DjlibService::slotDeckTrackChanged(const QString& group,
        TrackPointer pNewTrack,
        TrackPointer pOldTrack) {
    Q_UNUSED(group);
    Q_UNUSED(pNewTrack);
    if (!pOldTrack) {
        return;
    }
    const TrackId trackId = pOldTrack->getId();
    for (auto it = m_deferred.constBegin(); it != m_deferred.constEnd(); ++it) {
        if (it->trackId == trackId) {
            // Still loaded in another deck: applyToTrack() defers again.
            enqueue(Item{Item::Kind::Retry, it.key(), {}, {}, {}, false});
        }
    }
}

DjlibService::Status DjlibService::status() const {
    Status status;
    status.libraryPath = m_libraryPath;
    status.watching = !m_libraryPath.isEmpty() && !m_watcher.directories().isEmpty();
    status.lastImport = m_lastImport;
    status.lastImportFile = m_lastImportFile;
    for (const auto& state : m_store.trackStates()) {
        if (state.pendingJson) {
            ++status.waitingForPackage;
        }
    }
    status.deferred = static_cast<int>(m_deferred.size());
    status.queued = static_cast<int>(m_queue.size()) + (m_passRunning ? 1 : 0) + m_jobsRunning;
    return status;
}
