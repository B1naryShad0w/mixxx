#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "library/djlib/djlibcratesync.h"
#include "library/djlib/djlibimporter.h"
#include "library/djlib/djlibsidecar.h"
#include "library/djlib/djlibstore.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "track/trackid.h"

class TrackCollectionManager;

namespace djlib {

/// Size and modification time of a watched file (the snapshot diff key).
/// `racy`: seen so soon after its modification time that another write may
/// leave both unchanged; `hash` (sidecars, crates.json) then tells on the
/// next pass. Neither takes part in the comparison.
struct FileStamp {
    qint64 size = -1;
    qint64 mtimeMs = 0;
    bool racy = false;
    QByteArray hash;
    friend bool operator==(const FileStamp& lhs, const FileStamp& rhs) {
        return lhs.size == rhs.size && lhs.mtimeMs == rhs.mtimeMs;
    }
    friend bool operator!=(const FileStamp& lhs, const FileStamp& rhs) {
        return !(lhs == rhs);
    }
};

/// Watcher timing (tests use shorter values).
struct ServiceTiming {
    int debounceMillis = 1500;
    int maxDelayMillis = 10000; // force a pass after this much continuous activity
    int safetyPassMillis = 60000;
    int retryMinMillis = 1000; // after a read failure; doubles up to retryMaxMillis
    int retryMaxMillis = 60000;
    // An existing but empty tracks/ must still be empty this much later
    // before it counts as "every package removed".
    int emptyConfirmMillis = 60000;
};

/// Tests only: makes reading `path` fail (as a sharing violation would)
/// while `fn(path)` returns true. Called on worker threads; may throw.
void setReadFailureForTesting(std::function<bool(const QString& path)> fn);

/// Tests only: makes a library write fail (as a busy database would) while
/// `fn(what)` returns true; `what` is "save" (a track and its djlib state)
/// or "add" (a new library track). Called on the main thread.
void setWriteFailureForTesting(std::function<bool(const QString& what)> fn);

/// A sidecar read and parsed on a worker thread.
struct ParsedSidecar {
    QString sidecarName; // file name in <lib>/tracks
    bool packageChanged = false; // its package is new or was replaced
    QByteArray json;
    DjlibSidecar sidecar;
};

/// The crates of one sidecar on disk (crates.json membership re-apply).
struct SidecarCrates {
    QString djlibId;
    QStringList crateIds;
};

/// Result of one directory pass, computed on a worker thread.
struct PassResult {
    quint64 generation = 0;
    bool failed = false; // the pass threw: nothing below is set
    // false: tracks/ missing or unreadable; `snapshot` is the old one then
    // and nothing counts as removed.
    bool tracksDirOk = false;
    bool libraryDirOk = false; // the library folder exists
    bool djlibDirOk = false;   // <lib>/_djlib exists
    QHash<QString, FileStamp> snapshot; // name -> stamp, filtered names only
    std::optional<FileStamp> cratesStamp;
    std::optional<QList<CrateRef>> crates; // set iff changed and valid
    QStringList addedPackages; // new or changed, without a sidecar
    QList<ParsedSidecar> sidecars;
    QList<SidecarCrates> memberships; // all sidecars with a package, iff crates changed
    bool lostNames = false;           // a package of the old snapshot is gone
    QStringList readFailures;         // not in the snapshot/stamp: retried
};

} // namespace djlib

/// Watches a djlib library folder (`<lib>/tracks`, `<lib>/_djlib`) and applies
/// the sidecars the agent writes there (research §1, §6, §9).
///
/// - `QFileSystemWatcher` on both directories; events restart a debounce timer
///   (1.5 s, but a pass is forced after 10 s of continuous events), plus a
///   periodic safety pass (60 s) and a full pass at start.
/// - A pass lists the directories and diffs them against a snapshot
///   (name -> size, mtime) on a worker thread; new/changed sidecars (and the
///   sidecars of new/changed packages) and crates.json are read and parsed
///   there too. A sidecar read within 3 s of its modification time is
///   compared by content once more on the next pass. All file system access
///   to the library folder happens there; the main thread only adds the
///   watches and looks at the results. Only `*.stem.mp4` and `*.djlib.json` are considered; names
///   with a ".tmp<digits>" component (the agent's temp files) are ignored.
///   Files are only stat'ed and read (at most `djlib::kMaxJsonBytes`), never
///   held open or changed.
/// - The results go into a queue that is processed on the main thread, one
///   item per event-loop turn (library, DAO, crates and own SQL are main-thread
///   only). crates.json is reconciled first; on every crates.json change the
///   membership of all mapped tracks is re-applied from the sidecars on disk.
/// - Sidecar without package: kept as `pending_json` until the package shows
///   up. Package without sidecar: imported so it shows in the library; it waits
///   for its sidecar. Package gone (also while Mixxx was closed: every pass
///   compares the `djlib_tracks` rows with the listing): dropped from the
///   managed crates, the track stays in the library. A missing/unreadable
///   tracks/ dir is not "everything removed": the old snapshot is kept.
/// - A file that can't be read (not: invalid) isn't recorded and is retried
///   with a backoff (1 s doubling to 60 s).
/// - Library writes: the queue rests while the library scanner runs (it
///   keeps the database locked). A sidecar's track is saved first and its
///   `djlib_tracks` row stored only then; if either fails both are retried
///   with the same backoff, and so is a track that could not be added.
///   The row's `file` is the package's name on disk.
/// - Beats/cues/key of a track loaded in a deck are deferred until the deck
///   unloads it (`slotDeckTrackChanged`); an unknown sample rate is probed on a
///   worker (`SoundSourceProxy::openAudioSource`) and the import completed.
/// - `<lib>/tracks` is registered once as a library root directory.
/// - Renamed/moved files (same djlib track id, new file or library folder): if
///   the mapped library track's file is gone and the new file isn't in the
///   library yet, the row is relocated (`TrackCollectionManager::relocateTrack`)
///   so cues, edits and history stay. Otherwise the new file is a new library
///   track: the old one leaves the managed crates and the djlib state starts
///   fresh (old fingerprints would read as DJ edits).
///
/// Settings: `[djlib] LibraryPath` (empty = disabled). Main thread only.
class DjlibService : public QObject {
    Q_OBJECT
  public:
    using Timing = djlib::ServiceTiming;

    struct Status {
        QString libraryPath; // empty: disabled
        bool watching = false;
        QDateTime lastImport; // invalid: none since start
        QString lastImportFile;
        int trackFiles = 0;        // packages in tracks/ at the last pass
        QString problem;           // e.g. no tracks/ folder in the chosen path
        int waitingForPackage = 0; // sidecars without package
        int deferred = 0;          // waiting for a deck to unload / a sample rate
        int queued = 0;            // queue items and writes not done yet
    };

    static const ConfigKey kLibraryPathKey; // [djlib] LibraryPath

    using TrackLoadedFn = std::function<bool(const TrackPointer&)>;

    DjlibService(UserSettingsPointer pConfig,
            TrackCollectionManager* pTrackCollectionManager,
            QObject* pParent = nullptr,
            Timing timing = Timing());
    ~DjlibService() override;

    /// The running service (for the preferences page), or nullptr.
    static DjlibService* instance();

    /// `<musicDir>/djlib` (the agent's default library) if it exists, else
    /// empty. Used when `[djlib] LibraryPath` is absent (first run).
    static QString defaultLibraryPath(const QString& musicDir);
    static QString defaultLibraryPath();
    /// Cleans a chosen path; a picked `.../tracks` folder next to `_djlib`
    /// becomes its parent (the library folder).
    static QString normalizeLibraryPath(const QString& path);

    /// Tells whether a track is loaded in a deck/sampler (default: never).
    /// Wired to `PlayerInfo::isTrackLoaded` in CoreServices.
    void setTrackLoadedFn(TrackLoadedFn fn);

    /// Reads `[djlib] LibraryPath` and (re)starts watching. If the setting is
    /// absent, uses (and stores) `defaultLibraryPath()` when that exists.
    void start();

    /// Stores the path in the settings and re-arms the watcher.
    void setLibraryPath(const QString& libraryPath);
    QString libraryPath() const {
        return m_libraryPath;
    }

    Status status() const;

    DjlibStore& store() {
        return m_store;
    }

  public slots:
    /// `PlayerInfo::trackChanged`: completes deferred imports of the old track.
    void slotDeckTrackChanged(const QString& group,
            TrackPointer pNewTrack,
            TrackPointer pOldTrack);

    /// Runs a pass now (also used by the safety timer).
    void requestPass();

  signals:
    void statusChanged();
    /// A sidecar was applied completely (nothing deferred).
    void sidecarApplied(const QString& djlibId, qint64 sidecarVersion);
    /// Queue drained, no pass or probe running.
    void idle();

  private:
    struct Item {
        enum class Kind {
            Reconcile,
            Sweep,
            PackageAdded,
            Sidecar,
            Membership,
            Retry,
        };
        Kind kind;
        QString name; // package/sidecar file name, or djlib id (Membership, Retry)
        djlib::ParsedSidecar sidecar;
        QList<djlib::CrateRef> crates;
        QStringList crateIds;
        bool membershipsIncluded = false;
        int attempts = 0; // failed so far
    };

    struct Deferred {
        DjlibSidecar sidecar;
        TrackId trackId;
        QString packageName;
        DjlibImporter::Baseline baseline;
    };

    /// An applied sidecar whose track or row could not be written yet. Holds
    /// the track so the applied values stay in memory until they are saved.
    struct Unsaved {
        TrackPointer pTrack;
        DjlibStore::TrackState state;
        qint64 appliedVersion = 0; // > 0: announce as applied once written
    };

    void rearm();
    void registerLibraryRoot();
    bool watchDirectory(const QString& dir);
    void slotDirectoryChanged();
    void startPass();
    void onPassFinished(djlib::PassResult result);
    void startMembershipJob(const QSet<QString>& crateIds);
    void startProbe(const TrackPointer& pTrack);

    void enqueue(Item item);
    void processNext();
    bool process(const Item& item); // false: failed, try again later
    void slotLibraryScanStarted();
    void slotLibraryScanFinished();
    void scheduleWriteRetry();
    void slotWriteRetry();
    bool commit(const QString& djlibId, const Unsaved& write);
    void commitUnsaved();
    TrackPointer addTrack(const QString& path);
    void reconcile(const QList<djlib::CrateRef>& crates, bool membershipsIncluded);
    void sweepMissingPackages();
    void relocateIfMoved(TrackId trackId, const QString& newPath);
    bool packageAdded(const QString& packageName);
    bool sidecarChanged(const djlib::ParsedSidecar& parsed);
    void applyMembership(const QString& djlibId, const QStringList& crateIds);
    void retry(const QString& djlibId);
    void applyToTrack(const TrackPointer& pTrack,
            const DjlibSidecar& sidecar,
            const QString& packageName);
    bool isTrackLoaded(const TrackPointer& pTrack) const;
    QSet<QString> allowedCrateIds() const;
    void scheduleReconcile();
    void checkIdle();

    QString tracksDir() const;
    QString djlibDir() const;

    const UserSettingsPointer m_pConfig;
    TrackCollectionManager* const m_pTrackCollectionManager;
    const Timing m_timing;

    DjlibStore m_store;
    DjlibCrateSync m_crateSync;
    DjlibImporter m_importer;
    TrackLoadedFn m_trackLoadedFn;

    QString m_libraryPath;
    QString m_registeredRoot;
    quint64 m_generation = 0;

    QFileSystemWatcher m_watcher;
    QTimer m_debounceTimer;
    QTimer m_safetyTimer;
    QTimer m_queueTimer;
    QTimer m_retryTimer;
    QTimer m_writeRetryTimer;
    QElapsedTimer m_burst;
    QThreadPool m_pool;      // passes and membership jobs
    QThreadPool m_probePool; // sample-rate probes (never delay a pass)

    bool m_passRunning = false;
    bool m_passRequested = false;
    int m_jobsRunning = 0;
    bool m_reconcileQueued = false;
    bool m_reconciling = false;
    bool m_tracksDirMissing = false;
    bool m_libraryDirMissing = false;
    int m_retryDelayMillis = 0;
    int m_writeRetryDelayMillis = 0;
    bool m_scanRunning = false; // library scan: no library writes meanwhile
    bool m_sweepNeeded = false; // first pass after rearm, or names were lost
    QElapsedTimer m_emptySince; // tracks/ listed empty while rows are mapped
    QTimer m_confirmTimer;
    int m_packageCount = 0;
    std::shared_ptr<std::atomic<bool>> m_pStopping;

    QHash<QString, djlib::FileStamp> m_snapshot;
    std::optional<djlib::FileStamp> m_cratesStamp;
    std::optional<QList<djlib::CrateRef>> m_crates; // last valid crates.json

    QQueue<Item> m_queue;
    QList<Item> m_failedItems;         // re-queued by m_writeRetryTimer
    QHash<QString, Unsaved> m_unsaved; // djlib id -> write to repeat
    QHash<QString, Deferred> m_deferred; // djlib id -> sidecar
    QSet<TrackId> m_probing;
    QSet<TrackId> m_probed;

    QDateTime m_lastImport;
    QString m_lastImportFile;
};
