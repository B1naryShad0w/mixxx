#include "library/djlib/djlibservice.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtConcurrent>
#include <algorithm>
#include <utility>

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

std::function<bool(const QString&)> s_readFailureForTesting;
std::function<bool(const QString&)> s_writeFailureForTesting;

/// A queue item that failed (track not added, row not written) is tried
/// this often before it waits for the next change of its file.
constexpr int kMaxItemAttempts = 10;

bool writeFailsForTesting(const QString& what) {
    return s_writeFailureForTesting && s_writeFailureForTesting(what);
}

bool isPackageName(const QString& name) {
    return name.endsWith(kPackageSuffix, Qt::CaseInsensitive);
}

bool isSidecarName(const QString& name) {
    return name.endsWith(kSidecarSuffix, Qt::CaseInsensitive);
}

/// A temp file of the agent ("<target>.tmp<digits>") or a file named after
/// one. Only a whole ".tmp<digits>" name component counts: a title like
/// "Song.tmp edit" is an ordinary track.
bool isTempName(const QString& name) {
    static const QRegularExpression kTempComponent(
            QStringLiteral("\\.tmp[0-9]*(\\.|$)"),
            QRegularExpression::CaseInsensitiveOption);
    return kTempComponent.match(name).hasMatch();
}

/// Only packages and sidecars; never temp files of the agent.
bool isWatchedName(const QString& name) {
    return (isPackageName(name) || isSidecarName(name)) && !isTempName(name);
}

QString packageNameOf(const QString& sidecarName) {
    return sidecarName.chopped(kSidecarSuffix.size()) + kPackageSuffix;
}

QString sidecarNameOf(const QString& packageName) {
    return packageName.chopped(kPackageSuffix.size()) + kSidecarSuffix;
}

/// A file changed this recently may change again without a new size or
/// modification time (coarse timestamps: 2 s on FAT, cached on network
/// shares). Such sidecars are compared by content on the next pass.
constexpr qint64 kRacyMillis = 3000;

FileStamp stampOf(const QFileInfo& info, qint64 nowMs) {
    FileStamp stamp;
    stamp.size = info.size();
    stamp.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    stamp.racy = nowMs - stamp.mtimeMs < kRacyMillis;
    return stamp;
}

QByteArray contentHash(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha1);
}

/// Reads a whole small file; the handle is closed before returning. Never
/// more than one byte over `djlib::kMaxJsonBytes` (the parsers reject that
/// as too large), whatever size the file has.
std::optional<QByteArray> readFile(const QString& path) {
    if (s_readFailureForTesting && s_readFailureForTesting(path)) {
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.read(djlib::kMaxJsonBytes + 1);
}

enum class ReadResult {
    Ok,
    ReadFailed, // transient (sharing violation, ...): retry
    Invalid,    // retried only when the file changes
};

ReadResult readSidecar(const QString& path, DjlibSidecar* pSidecar, QByteArray* pJson) {
    const auto json = readFile(path);
    if (!json) {
        kLogger.warning() << "Cannot read" << path << "- retrying later";
        return ReadResult::ReadFailed;
    }
    if (pJson) {
        *pJson = *json; // also of an invalid file (content hash)
    }
    QString error;
    auto sidecar = DjlibSidecar::parse(*json, &error);
    if (!sidecar) {
        kLogger.warning() << "Invalid sidecar" << path << error;
        return ReadResult::Invalid;
    }
    *pSidecar = std::move(*sidecar);
    return ReadResult::Ok;
}

bool isUsableDir(const QString& path) {
    const QFileInfo info(path);
    return info.isDir() && info.isReadable();
}

QHash<QString, FileStamp> listTracksDir(const QString& tracksDir) {
    QHash<QString, FileStamp> result;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const auto entries = QDir(tracksDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const auto& info : entries) {
        if (isWatchedName(info.fileName())) {
            result.insert(info.fileName(), stampOf(info, nowMs));
        }
    }
    return result;
}

SidecarCrates cratesOf(const DjlibSidecar& sidecar) {
    SidecarCrates entry{sidecar.trackId, {}};
    for (const auto& crate : sidecar.crates) {
        entry.crateIds.append(crate.id);
    }
    return entry;
}

/// The crates of every sidecar whose package is present (all of them, or only
/// those listing one of `onlyCrateIds`). `known`: sidecars already parsed.
QList<SidecarCrates> collectMembershipsUnguarded(const QString& tracksDir,
        const QHash<QString, FileStamp>& snapshot,
        const QSet<QString>& onlyCrateIds,
        const QHash<QString, SidecarCrates>& known,
        const std::shared_ptr<std::atomic<bool>>& pStopping) {
    QList<SidecarCrates> result;
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        if (pStopping->load()) {
            return {};
        }
        if (!isSidecarName(it.key()) || !snapshot.contains(packageNameOf(it.key()))) {
            continue;
        }
        SidecarCrates entry;
        const auto knownIt = known.constFind(it.key());
        if (knownIt != known.constEnd()) {
            entry = *knownIt;
        } else {
            DjlibSidecar sidecar;
            if (readSidecar(QDir(tracksDir).filePath(it.key()), &sidecar, nullptr) !=
                    ReadResult::Ok) {
                continue;
            }
            entry = cratesOf(sidecar);
        }
        bool relevant = onlyCrateIds.isEmpty();
        for (const auto& id : std::as_const(entry.crateIds)) {
            relevant |= onlyCrateIds.contains(id);
        }
        if (relevant) {
            result.append(entry);
        }
    }
    return result;
}

/// Worker jobs must always deliver a result: an exception (out of memory on
/// a broken file, ...) lost in the thread pool would leave the service
/// waiting for the job forever.
QList<SidecarCrates> collectMemberships(const QString& tracksDir,
        const QHash<QString, FileStamp>& snapshot,
        const QSet<QString>& onlyCrateIds,
        const QHash<QString, SidecarCrates>& known,
        const std::shared_ptr<std::atomic<bool>>& pStopping) {
    try {
        return collectMembershipsUnguarded(tracksDir, snapshot, onlyCrateIds, known, pStopping);
    } catch (...) {
        kLogger.warning() << "Reading the sidecars in" << tracksDir << "failed";
        return {};
    }
}

/// One pass on a worker thread: list, diff against `snapshot`, read and parse
/// what changed. Touches no library state.
PassResult runPassUnguarded(quint64 generation,
        const QString& libraryPath,
        const QHash<QString, FileStamp>& snapshot,
        const std::optional<FileStamp>& cratesStamp,
        const std::shared_ptr<std::atomic<bool>>& pStopping) {
    const QString tracksDir = QDir(libraryPath).filePath(kTracksDir);
    const QString djlibDir = QDir(libraryPath).filePath(kDjlibDir);
    const QString cratesPath = QDir(djlibDir).filePath(kCratesFile);
    PassResult result;
    result.generation = generation;
    result.libraryDirOk = QFileInfo(libraryPath).isDir();
    result.djlibDirOk = QFileInfo(djlibDir).isDir();
    result.tracksDirOk = isUsableDir(tracksDir);
    // A missing/unreadable dir (unplugged drive, dropped share) is not
    // "every package removed": keep the old snapshot.
    result.snapshot = result.tracksDirOk ? listTracksDir(tracksDir) : snapshot;

    result.cratesStamp = cratesStamp;
    const QFileInfo cratesInfo(cratesPath);
    if (cratesInfo.exists()) {
        FileStamp stamp = stampOf(cratesInfo, QDateTime::currentMSecsSinceEpoch());
        // Same stamp as a file read right after it was written: compare the
        // content.
        const bool recheck = cratesStamp && stamp == *cratesStamp && cratesStamp->racy;
        if (stamp != cratesStamp || recheck) {
            const auto json = readFile(cratesPath);
            if (!json) {
                // Not adopting the stamp: retried on the next pass.
                kLogger.warning() << "Cannot read" << cratesPath << "- retrying later";
                result.readFailures.append(cratesPath);
            } else {
                stamp.hash = contentHash(*json);
                result.cratesStamp = stamp;
                if (!recheck || stamp.hash != cratesStamp->hash) {
                    QString error;
                    result.crates = djlib::parseCratesJson(*json, &error);
                    if (!result.crates) {
                        // Never reconcile on an invalid file: that would delete
                        // every managed crate. Retried when the file changes.
                        kLogger.warning() << "Invalid" << cratesPath << error;
                    }
                }
            }
        }
    }
    if (!result.tracksDirOk) {
        return result;
    }

    QSet<QString> sidecarsToRead;
    QSet<QString> sidecarsToRecheck; // unchanged stamp, but read while racy
    QStringList changedPackages;
    for (auto it = result.snapshot.begin(); it != result.snapshot.end(); ++it) {
        const auto previous = snapshot.constFind(it.key());
        if (previous != snapshot.constEnd() && *previous == it.value()) {
            it->hash = previous->hash;
            if (previous->racy && isSidecarName(it.key())) {
                sidecarsToRecheck.insert(it.key());
            }
            continue;
        }
        if (isPackageName(it.key())) {
            changedPackages.append(it.key());
            // A sidecar that may have waited for this package.
            const QString sidecarName = sidecarNameOf(it.key());
            if (result.snapshot.contains(sidecarName)) {
                sidecarsToRead.insert(sidecarName);
            }
        } else {
            sidecarsToRead.insert(it.key());
        }
    }
    sidecarsToRecheck.subtract(sidecarsToRead);
    QStringList sidecarNames(sidecarsToRead.cbegin(), sidecarsToRead.cend());
    sidecarNames.append(QStringList(sidecarsToRecheck.cbegin(), sidecarsToRecheck.cend()));
    sidecarNames.sort();
    QSet<QString> handledPackages; // a sidecar item imports them
    QHash<QString, SidecarCrates> known;
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        if (isPackageName(it.key()) && !result.snapshot.contains(it.key())) {
            result.lostNames = true;
            break;
        }
    }
    for (const auto& name : std::as_const(sidecarNames)) {
        if (pStopping->load()) {
            return result; // shutting down: the result is discarded
        }
        const bool recheck = sidecarsToRecheck.contains(name);
        ParsedSidecar parsed;
        parsed.sidecarName = name;
        parsed.packageChanged = changedPackages.contains(packageNameOf(name));
        const ReadResult read = readSidecar(
                QDir(tracksDir).filePath(name), &parsed.sidecar, &parsed.json);
        if (read == ReadResult::ReadFailed) {
            if (recheck) {
                // Compared again on the next pass.
                result.snapshot[name].racy = true;
                continue;
            }
            // Not in the snapshot: it shows up as new on the next pass.
            result.snapshot.remove(name);
            result.readFailures.append(name);
            handledPackages.insert(packageNameOf(name));
            continue;
        }
        const QByteArray hash = contentHash(parsed.json);
        const bool sameContent = recheck && hash == result.snapshot.value(name).hash;
        result.snapshot[name].hash = hash;
        if (read == ReadResult::Ok && !sameContent) {
            handledPackages.insert(packageNameOf(name));
            known.insert(name, cratesOf(parsed.sidecar));
            result.sidecars.append(std::move(parsed));
        }
    }
    changedPackages.sort();
    for (const auto& name : std::as_const(changedPackages)) {
        if (!handledPackages.contains(name)) {
            result.addedPackages.append(name);
        }
    }
    if (result.crates) {
        result.memberships = collectMembershipsUnguarded(
                tracksDir, result.snapshot, {}, known, pStopping);
    }
    return result;
}

PassResult runPass(quint64 generation,
        const QString& libraryPath,
        const QHash<QString, FileStamp>& snapshot,
        const std::optional<FileStamp>& cratesStamp,
        const std::shared_ptr<std::atomic<bool>>& pStopping) {
    try {
        return runPassUnguarded(generation, libraryPath, snapshot, cratesStamp, pStopping);
    } catch (...) {
        kLogger.warning() << "The pass over" << libraryPath << "failed - retrying later";
        PassResult result;
        result.generation = generation;
        result.failed = true;
        return result;
    }
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
          m_importer(&m_crateSync),
          m_pStopping(std::make_shared<std::atomic<bool>>(false)) {
    if (!m_store.initialize()) {
        kLogger.warning() << "Cannot create the djlib tables";
    }
    // One pass/membership job at a time; probes have their own pool so 200
    // new packages never delay the next pass.
    m_pool.setMaxThreadCount(1);
    m_probePool.setMaxThreadCount(1);
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &DjlibService::requestPass);
    m_writeRetryTimer.setSingleShot(true);
    connect(&m_writeRetryTimer, &QTimer::timeout, this, &DjlibService::slotWriteRetry);
    // The library scanner writes the database from its own thread and keeps
    // it locked for long stretches: our writes would fail. Wait for it.
    connect(m_pTrackCollectionManager,
            &TrackCollectionManager::libraryScanStarted,
            this,
            &DjlibService::slotLibraryScanStarted);
    connect(m_pTrackCollectionManager,
            &TrackCollectionManager::libraryScanFinished,
            this,
            &DjlibService::slotLibraryScanFinished);
    m_confirmTimer.setSingleShot(true);
    connect(&m_confirmTimer, &QTimer::timeout, this, &DjlibService::requestPass);

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
                    for (auto it = m_unsaved.begin(); it != m_unsaved.end();) {
                        it = purged.contains(it->state.trackId) ? m_unsaved.erase(it) : ++it;
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
    commitUnsaved(); // last chance: the tracks themselves are saved on exit
    m_pStopping->store(true); // running passes stop reading sidecars
    // Probes not started yet are dropped, the running one is waited for
    // (one decoder open; it can't be interrupted).
    m_probePool.clear();
    m_pool.waitForDone();
    m_probePool.waitForDone();
}

void djlib::setReadFailureForTesting(std::function<bool(const QString& path)> fn) {
    s_readFailureForTesting = std::move(fn);
}

void djlib::setWriteFailureForTesting(std::function<bool(const QString& what)> fn) {
    s_writeFailureForTesting = std::move(fn);
}

// static
DjlibService* DjlibService::instance() {
    return s_pInstance;
}

void DjlibService::setTrackLoadedFn(TrackLoadedFn fn) {
    m_trackLoadedFn = std::move(fn);
}

// static
QString DjlibService::defaultLibraryPath(const QString& musicDir) {
    if (musicDir.isEmpty()) {
        return QString();
    }
    const QString path = QDir(musicDir).filePath(QStringLiteral("djlib"));
    return QFileInfo(path).isDir() ? QDir::cleanPath(path) : QString();
}

// static
QString DjlibService::defaultLibraryPath() {
    // Windows: %USERPROFILE%\Music\djlib, the agent's default library.
    return defaultLibraryPath(QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
}

// static
QString DjlibService::normalizeLibraryPath(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(trimmed));
    const QFileInfo info(clean);
    // A DJ picking <lib>/tracks instead of <lib>.
    if (info.fileName().compare(kTracksDir, Qt::CaseInsensitive) == 0) {
        const QDir parent = info.dir();
        if (QFileInfo(parent.filePath(kDjlibDir)).isDir()) {
            return QDir::cleanPath(parent.path());
        }
    }
    return clean;
}

void DjlibService::start() {
    if (m_pConfig->exists(kLibraryPathKey)) {
        m_libraryPath = normalizeLibraryPath(m_pConfig->getValueString(kLibraryPathKey));
    } else {
        // First run: the agent's default folder, if it is there.
        m_libraryPath = defaultLibraryPath();
        if (!m_libraryPath.isEmpty()) {
            kLogger.info() << "Using the default djlib library" << m_libraryPath;
            m_pConfig->setValue(kLibraryPathKey, m_libraryPath);
        }
    }
    rearm();
}

void DjlibService::setLibraryPath(const QString& libraryPath) {
    const QString path = normalizeLibraryPath(libraryPath);
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
    m_retryTimer.stop();
    m_retryDelayMillis = 0;
    m_writeRetryTimer.stop();
    m_writeRetryDelayMillis = 0;
    commitUnsaved();
    m_unsaved.clear();
    m_failedItems.clear();
    m_tracksDirMissing = false;
    m_libraryDirMissing = false;
    m_confirmTimer.stop();
    m_emptySince.invalidate();
    m_sweepNeeded = true;
    m_packageCount = 0;
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
    if (m_registeredRoot == dir) {
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

bool DjlibService::watchDirectory(const QString& dir) {
    // directories() is the watcher's own list: no file system access.
    return !m_watcher.directories().contains(dir) && m_watcher.addPath(dir);
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
    // The main thread never asks the file system about the library folder
    // (a sleeping disk or a dead share would freeze the UI): the pass does,
    // and onPassFinished() watches and registers what it found.
    m_passRunning = true;
    QtConcurrent::run(&m_pool,
            runPass,
            m_generation,
            m_libraryPath,
            m_snapshot,
            m_cratesStamp,
            m_pStopping)
            .then(this, [this](PassResult result) {
                onPassFinished(std::move(result));
            });
}

void DjlibService::onPassFinished(PassResult result) {
    m_passRunning = false;
    if (result.generation == m_generation && result.failed) {
        // Nothing learned: keep the snapshot and try again with the backoff.
        m_retryDelayMillis = m_retryDelayMillis == 0
                ? m_timing.retryMinMillis
                : std::min(2 * m_retryDelayMillis, m_timing.retryMaxMillis);
        m_retryTimer.start(m_retryDelayMillis);
    } else if (result.generation == m_generation) {
        if (result.tracksDirOk == m_tracksDirMissing) {
            m_tracksDirMissing = !result.tracksDirOk;
            if (m_tracksDirMissing) {
                kLogger.warning() << tracksDir()
                                  << "is missing or unreadable - keeping the djlib "
                                     "crates as they are until it is back";
            } else {
                kLogger.info() << tracksDir() << "is available";
            }
        }
        m_libraryDirMissing = !result.libraryDirOk;
        // Directories may only appear with the first sync. Something may
        // have changed between the listing and the watch: look once more.
        bool newlyWatched = false;
        if (result.tracksDirOk) {
            if (!m_scanRunning) {
                registerLibraryRoot(); // else: with the pass after the scan
            }
            newlyWatched |= watchDirectory(tracksDir());
        }
        if (result.djlibDirOk) {
            newlyWatched |= watchDirectory(djlibDir());
        }
        m_passRequested |= newlyWatched;
        m_snapshot = std::move(result.snapshot);
        m_cratesStamp = result.cratesStamp;
        m_packageCount = static_cast<int>(std::count_if(m_snapshot.keyBegin(),
                m_snapshot.keyEnd(),
                [](const QString& name) { return isPackageName(name); }));
        // crates.json first: memberships need the managed crates. Without a
        // usable tracks/ no memberships were collected.
        if (result.crates) {
            enqueue(Item{Item::Kind::Reconcile,
                    {},
                    {},
                    *result.crates,
                    {},
                    result.tracksDirOk});
        }
        // Packages gone, also while Mixxx was closed (persisted rows vs
        // listing): on the first pass and whenever names were lost.
        m_sweepNeeded |= result.lostNames;
        if (result.tracksDirOk && m_sweepNeeded) {
            enqueue(Item{Item::Kind::Sweep, {}, {}, {}, {}, false});
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
        // Read failures (sharing violation, AV scanner, ...): retry with backoff.
        if (result.readFailures.isEmpty()) {
            m_retryDelayMillis = 0;
        } else {
            m_retryDelayMillis = m_retryDelayMillis == 0
                    ? m_timing.retryMinMillis
                    : std::min(2 * m_retryDelayMillis, m_timing.retryMaxMillis);
            m_retryTimer.start(m_retryDelayMillis);
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
    QtConcurrent::run(&m_pool,
            collectMemberships,
            tracksDir(),
            m_snapshot,
            crateIds,
            QHash<QString, SidecarCrates>(),
            m_pStopping)
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
    QtConcurrent::run(&m_probePool,
            [pTrack, pStopping = m_pStopping] {
                if (pStopping->load()) {
                    return;
                }
                try {
                    SoundSourceProxy(pTrack).openAudioSource();
                } catch (...) {
                    kLogger.warning() << "Probing" << pTrack->getLocation() << "failed";
                }
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
    // Our own deletes during reconcile() emit crateDeleted too: ignore them.
    if (m_reconcileQueued || m_reconciling || !m_crates) {
        return;
    }
    m_reconcileQueued = true;
    enqueue(Item{Item::Kind::Reconcile, {}, {}, *m_crates, {}, false});
}

void DjlibService::enqueue(Item item) {
    m_queue.enqueue(std::move(item));
    if (!m_queueTimer.isActive() && !m_scanRunning) {
        m_queueTimer.start();
    }
}

void DjlibService::processNext() {
    if (m_scanRunning) {
        return; // resumed by slotLibraryScanFinished()
    }
    if (!m_queue.isEmpty()) {
        Item item = m_queue.dequeue();
        if (!process(item)) {
            // Usually the database was busy: try again later.
            if (++item.attempts < kMaxItemAttempts) {
                m_failedItems.append(std::move(item));
                scheduleWriteRetry();
            } else {
                kLogger.warning() << "Giving up on" << item.name
                                  << "until it changes or Mixxx restarts";
            }
        }
    }
    if (!m_queue.isEmpty()) {
        m_queueTimer.start(); // next item in the next event-loop turn
    } else {
        if (m_failedItems.isEmpty() && m_unsaved.isEmpty()) {
            m_writeRetryDelayMillis = 0;
        }
        emit statusChanged();
        checkIdle();
    }
}

void DjlibService::checkIdle() {
    if (m_queue.isEmpty() && !m_passRunning && m_jobsRunning == 0 &&
            m_failedItems.isEmpty() && m_unsaved.isEmpty()) {
        emit idle();
    }
}

void DjlibService::slotLibraryScanStarted() {
    m_scanRunning = true;
    m_queueTimer.stop();
    m_writeRetryTimer.stop();
    emit statusChanged();
}

void DjlibService::slotLibraryScanFinished() {
    if (!m_scanRunning) {
        return;
    }
    m_scanRunning = false;
    if (!m_queue.isEmpty()) {
        m_queueTimer.start();
    }
    if (!m_failedItems.isEmpty() || !m_unsaved.isEmpty()) {
        m_writeRetryDelayMillis = 0;
        scheduleWriteRetry();
    }
    // What the scan made of the folder, and the root if it wasn't added yet.
    startPass();
    emit statusChanged();
}

void DjlibService::scheduleWriteRetry() {
    if (m_scanRunning || m_writeRetryTimer.isActive()) {
        return;
    }
    m_writeRetryDelayMillis = m_writeRetryDelayMillis == 0
            ? m_timing.retryMinMillis
            : std::min(2 * m_writeRetryDelayMillis, m_timing.retryMaxMillis);
    m_writeRetryTimer.start(m_writeRetryDelayMillis);
}

void DjlibService::slotWriteRetry() {
    if (m_scanRunning) {
        return;
    }
    commitUnsaved();
    const QList<Item> failedItems = std::exchange(m_failedItems, {});
    for (const auto& item : failedItems) {
        enqueue(item);
    }
    if (!m_unsaved.isEmpty()) {
        scheduleWriteRetry();
    }
    emit statusChanged();
    checkIdle();
}

bool DjlibService::commit(const QString& djlibId, const Unsaved& write) {
    // The track first: fingerprints of values the library never got would
    // make the next sidecar take the old values for DJ edits.
    if (writeFailsForTesting(QStringLiteral("save")) ||
            m_pTrackCollectionManager->saveTrack(write.pTrack) ==
                    TrackCollectionManager::SaveTrackResult::Failed) {
        kLogger.warning() << "Saving failed for" << djlibId << "- retrying later";
        return false;
    }
    if (!m_store.upsertTrackState(write.state)) {
        kLogger.warning() << "Storing the state of" << djlibId << "failed - retrying later";
        return false;
    }
    if (write.appliedVersion > 0) {
        m_lastImport = QDateTime::currentDateTime();
        m_lastImportFile = write.state.file;
        emit sidecarApplied(djlibId, write.appliedVersion);
    }
    return true;
}

void DjlibService::commitUnsaved() {
    const QStringList djlibIds = m_unsaved.keys();
    for (const auto& djlibId : djlibIds) {
        if (commit(djlibId, m_unsaved.value(djlibId))) {
            m_unsaved.remove(djlibId);
        }
    }
}

TrackPointer DjlibService::addTrack(const QString& path) {
    if (writeFailsForTesting(QStringLiteral("add"))) {
        return TrackPointer();
    }
    return m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(path));
}

bool DjlibService::process(const Item& item) {
    switch (item.kind) {
    case Item::Kind::Reconcile:
        reconcile(item.crates, item.membershipsIncluded);
        break;
    case Item::Kind::Sweep:
        sweepMissingPackages();
        break;
    case Item::Kind::PackageAdded:
        return packageAdded(item.name);
    case Item::Kind::Sidecar:
        return sidecarChanged(item.sidecar);
    case Item::Kind::Membership:
        applyMembership(item.name, item.crateIds);
        break;
    case Item::Kind::Retry:
        retry(item.name);
        break;
    }
    return true;
}

void DjlibService::reconcile(const QList<djlib::CrateRef>& crates, bool membershipsIncluded) {
    m_reconcileQueued = false;
    m_crates = crates;
    QStringList createdIds;
    m_reconciling = true;
    if (!m_crateSync.reconcile(crates, &createdIds)) {
        kLogger.warning() << "crates.json reconcile had errors";
    }
    m_reconciling = false;
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

void DjlibService::sweepMissingPackages() {
    // Compares the persisted rows with the current listing, so removals made
    // while Mixxx was closed are caught too. Only called with a usable
    // tracks/ dir. The track stays in the library (playlists, history) for
    // Mixxx's missing-track handling; only the djlib crates change.
    QList<DjlibStore::TrackFile> gone;
    const QString tracksPath = QDir(tracksDir()).absolutePath();
    for (const auto& entry : m_store.trackFiles()) {
        if (!entry.trackId.isValid() || entry.sidecarVersion < 0 ||
                m_snapshot.contains(entry.file)) {
            continue;
        }
        // The row may have the sidecar's "file" of an older version, which
        // can be behind the name on disk: the library knows where the track
        // really is.
        const QFileInfo location(m_pTrackCollectionManager->internalCollection()
                        ->getTrackDAO()
                        .getTrackLocation(entry.trackId));
        if (location.absolutePath() == tracksPath &&
                m_snapshot.contains(location.fileName())) {
            if (auto state = m_store.trackState(entry.djlibId)) {
                state->file = location.fileName();
                m_store.upsertTrackState(*state);
            }
            continue;
        }
        gone.append(entry);
    }
    if (gone.isEmpty()) {
        m_sweepNeeded = false;
        m_emptySince.invalidate();
        return;
    }
    // An existing but empty tracks/ (agent recreating it, a failed listing)
    // only counts once it is still empty a while later.
    if (m_packageCount == 0) {
        if (!m_emptySince.isValid()) {
            kLogger.warning() << tracksDir() << "is empty - keeping the djlib crates for now";
            m_emptySince.start();
        }
        if (m_emptySince.elapsed() < m_timing.emptyConfirmMillis) {
            m_confirmTimer.start(static_cast<int>(
                    m_timing.emptyConfirmMillis - m_emptySince.elapsed()));
            return; // m_sweepNeeded stays set
        }
    }
    m_sweepNeeded = false;
    m_emptySince.invalidate();
    for (const auto& entry : std::as_const(gone)) {
        kLogger.info() << entry.file << "is gone: leaving the djlib crates";
        m_crateSync.setMembership(entry.trackId, {});
        m_deferred.remove(entry.djlibId);
        // -1 = package gone: if it comes back (or under a new name), its
        // sidecar is applied again; the fingerprints stay, so DJ edits win.
        m_store.setSidecarVersion(entry.djlibId, -1);
    }
}

bool DjlibService::packageAdded(const QString& packageName) {
    if (!m_snapshot.contains(packageName)) {
        return true; // gone again meanwhile
    }
    // Import it so it shows in the library; its sidecar follows.
    const QString path = QDir(tracksDir()).filePath(packageName);
    const TrackPointer pTrack = addTrack(path);
    if (!pTrack) {
        kLogger.warning() << "Cannot add" << path;
        return false;
    }
    if (!pTrack->getSampleRate().isValid() && !isTrackLoaded(pTrack)) {
        startProbe(pTrack);
    }
    return true;
}

bool DjlibService::sidecarChanged(const ParsedSidecar& parsed) {
    const DjlibSidecar& sidecar = parsed.sidecar;
    QString packageName = packageNameOf(parsed.sidecarName);
    if (!m_snapshot.contains(packageName) && m_snapshot.contains(sidecar.file)) {
        packageName = sidecar.file;
    }
    const auto existing = m_store.trackState(sidecar.trackId);
    if (!m_snapshot.contains(packageName)) {
        // Sidecar without package: wait for the package. A stale one (e.g.
        // left behind by an interrupted rename) isn't newer: ignore it.
        if (existing && existing->trackId.isValid() &&
                sidecar.sidecarVersion <= existing->sidecarVersion) {
            return true;
        }
        if (existing) {
            return m_store.setPendingJson(sidecar.trackId, parsed.json);
        }
        DjlibStore::TrackState state;
        state.djlibId = sidecar.trackId;
        state.file = sidecar.file;
        state.pendingJson = parsed.json;
        return m_store.upsertTrackState(state);
    }
    const QString path = QDir(tracksDir()).filePath(packageName);
    // Fast path (startup pass): nothing newer to apply and the mapped library
    // track still is this file (not moved), so no track needs loading.
    if (existing && existing->trackId.isValid() && !existing->pendingJson &&
            !m_deferred.contains(sidecar.trackId) &&
            !m_unsaved.contains(sidecar.trackId) &&
            sidecar.sidecarVersion <= existing->sidecarVersion &&
            m_pTrackCollectionManager->internalCollection()
                            ->getTrackDAO()
                            .getTrackLocation(existing->trackId) ==
                    mixxx::FileInfo(path).location()) {
        return true;
    }
    if (existing && existing->trackId.isValid()) {
        relocateIfMoved(existing->trackId, path);
    }
    const TrackPointer pTrack = addTrack(path);
    if (!pTrack) {
        kLogger.warning() << "Cannot add" << path;
        return false;
    }
    if (existing && existing->trackId.isValid() && existing->trackId != pTrack->getId()) {
        // Couldn't relocate (old file still there, or the new file already
        // was a library track): the new library track takes over. The old one
        // leaves the djlib crates; the old fingerprints describe the old
        // track and would read as DJ edits here, so start fresh.
        kLogger.info() << sidecar.trackId << "moved to" << path << "(new library track)";
        m_crateSync.setMembership(existing->trackId, {});
        DjlibStore::TrackState state = *existing;
        state.trackId = TrackId();
        state.sidecarVersion = 0;
        state.appliedBeats.clear();
        state.appliedCues.clear();
        state.appliedKey.reset();
        m_unsaved.remove(sidecar.trackId);
        m_deferred.remove(sidecar.trackId);
        if (!m_store.upsertTrackState(state)) {
            return false;
        }
    }
    applyToTrack(pTrack, sidecar, packageName);
    return true;
}

void DjlibService::relocateIfMoved(TrackId trackId, const QString& newPath) {
    const mixxx::FileInfo newFileInfo(newPath);
    const QString oldLocation =
            m_pTrackCollectionManager->internalCollection()->getTrackDAO().getTrackLocation(
                    trackId);
    if (oldLocation.isEmpty() || oldLocation == newFileInfo.location()) {
        return; // not in the library, or not moved
    }
    // Is the old file still there (a copy)? For a file in tracks/ the last
    // listing knows, with exact names: a rename that only changes the case
    // is a rename also on Windows. Only an old file elsewhere (library
    // folder moved) is looked up.
    const QFileInfo oldInfo(oldLocation);
    const bool oldStillThere = oldInfo.absolutePath() == QDir(tracksDir()).absolutePath()
            ? m_snapshot.contains(oldInfo.fileName())
            : oldInfo.exists();
    if (oldStillThere) {
        return;
    }
    // The new file may already be a library track: the package was listed
    // (and added) before its sidecar arrived. A row nobody has used yet is
    // dropped in favour of the old one; else the new track takes over.
    if (!dropUnusedDuplicate(trackId, newFileInfo)) {
        return;
    }
    // Renamed by the agent (title fix) or library folder moved: keep the
    // row with all DJ edits, cues and history.
    if (!m_pTrackCollectionManager->relocateTrack(trackId, newFileInfo)) {
        kLogger.warning() << "Cannot relocate" << oldLocation << "->" << newPath;
    }
}

bool DjlibService::dropUnusedDuplicate(TrackId keptTrackId, const mixxx::FileInfo& fileInfo) {
    TrackId duplicateId;
    {
        const TrackPointer pDuplicate =
                m_pTrackCollectionManager->getTrackByRef(TrackRef::fromFileInfo(fileInfo));
        if (!pDuplicate) {
            return true; // no row in the way
        }
        duplicateId = pDuplicate->getId();
        if (!duplicateId.isValid() || duplicateId == keptTrackId) {
            return false;
        }
        const auto cues = pDuplicate->getCuePoints();
        const bool hasHotCues = std::any_of(cues.cbegin(), cues.cend(), [](const CuePointer& pCue) {
            return pCue && pCue->getType() == mixxx::CueType::HotCue;
        });
        if (isTrackLoaded(pDuplicate) || pDuplicate->getTimesPlayed() > 0 ||
                hasHotCues || m_store.trackStateByTrackId(duplicateId)) {
            return false;
        }
    }
    TrackCollection* const pCollection = m_pTrackCollectionManager->internalCollection();
    QSet<int> playlists;
    pCollection->getPlaylistDAO().getPlaylistsTrackIsIn(duplicateId, &playlists);
    if (!playlists.isEmpty() ||
            !pCollection->crates().collectCrateIdsOfTracks({duplicateId}).isEmpty()) {
        return false;
    }
    kLogger.info() << "Dropping the unused library entry of" << fileInfo.location()
                   << "- the renamed track keeps its entry";
    m_pTrackCollectionManager->purgeTracks({TrackRef::fromFileInfo(fileInfo, duplicateId)});
    if (m_pTrackCollectionManager->getTrackById(duplicateId)) {
        return false; // not purged
    }
    m_probed.remove(duplicateId);
    for (auto it = m_deferred.begin(); it != m_deferred.end();) {
        it = it->trackId == duplicateId ? m_deferred.erase(it) : ++it;
    }
    return true;
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
    applyToTrack(pTrack, deferred.sidecar, deferred.packageName);
}

void DjlibService::applyToTrack(const TrackPointer& pTrack,
        const DjlibSidecar& sidecar,
        const QString& packageName) {
    const QString& djlibId = sidecar.trackId;
    const TrackId trackId = pTrack->getId();
    const bool loaded = isTrackLoaded(pTrack);
    // A row that could not be stored yet says what the track has now.
    std::optional<DjlibStore::TrackState> existing;
    const auto unsaved = m_unsaved.constFind(djlibId);
    if (unsaved != m_unsaved.constEnd() && unsaved->state.trackId == trackId) {
        existing = unsaved->state;
    } else {
        existing = m_store.trackState(djlibId);
    }
    DjlibImporter::Baseline baseline;
    const auto deferred = m_deferred.constFind(djlibId);
    if (deferred != m_deferred.constEnd() && deferred->trackId == trackId) {
        baseline = deferred->baseline;
    }
    const auto result = m_importer.prepare(
            pTrack, sidecar, existing, loaded, allowedCrateIds(), baseline);
    if (result.outdated) {
        m_deferred.remove(djlibId);
        return; // a row not stored yet keeps waiting for its retry
    }
    Unsaved write;
    write.pTrack = pTrack;
    write.state = result.state;
    // The name on disk (what the listing is compared with), not the
    // sidecar's "file", which may be behind after a rename.
    write.state.file = packageName;
    write.state.pendingJson.reset();
    if (!result.hasDeferred()) {
        write.appliedVersion = sidecar.sidecarVersion;
    }
    // Save the track, then store the row; both again later if one failed.
    m_unsaved.remove(djlibId);
    if (!commit(djlibId, write)) {
        m_unsaved.insert(djlibId, write);
        scheduleWriteRetry();
    }
    if (result.hasDeferred()) {
        // Completed on deck unload (slotDeckTrackChanged) or after the probe.
        m_deferred.insert(djlibId, Deferred{sidecar, trackId, packageName, result.baseline});
        if (result.needsSampleRate && !loaded) {
            if (m_probed.contains(trackId)) {
                kLogger.warning() << "Sample rate unknown for" << djlibId
                                  << "- beats/cues wait for a deck load";
            } else {
                startProbe(pTrack);
            }
        }
    } else {
        m_deferred.remove(djlibId);
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
    status.trackFiles = m_packageCount;
    // As seen by the last pass (no file system access here).
    if (!m_libraryPath.isEmpty() && m_tracksDirMissing) {
        status.problem = m_libraryDirMissing
                ? QStringLiteral("no-library-folder")
                : QStringLiteral("no-tracks-folder");
    }
    status.waitingForPackage = m_store.countPending();
    status.deferred = static_cast<int>(m_deferred.size());
    status.queued = static_cast<int>(m_queue.size() + m_failedItems.size() + m_unsaved.size()) +
            (m_passRunning ? 1 : 0) + m_jobsRunning;
    return status;
}
