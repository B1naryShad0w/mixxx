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
#include <functional>
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
struct FileStamp {
    qint64 size = -1;
    qint64 mtimeMs = 0;
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
};

/// A sidecar read and parsed on a worker thread.
struct ParsedSidecar {
    QString sidecarName; // file name in <lib>/tracks
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
    QHash<QString, FileStamp> snapshot; // name -> stamp, filtered names only
    std::optional<FileStamp> cratesStamp;
    bool cratesChanged = false;
    std::optional<QList<CrateRef>> crates; // set iff changed and valid
    QStringList removedPackages;
    QStringList addedPackages; // new or changed
    QList<ParsedSidecar> sidecars;
    QList<SidecarCrates> memberships; // all sidecars with a package, iff crates changed
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
///   there too. Only `*.stem.mp4` and `*.djlib.json` are considered; names
///   containing ".tmp" are ignored. Files are only stat'ed and read, never
///   held open or changed.
/// - The results go into a queue that is processed on the main thread, one
///   item per event-loop turn (library, DAO, crates and own SQL are main-thread
///   only). crates.json is reconciled first; on every crates.json change the
///   membership of all mapped tracks is re-applied from the sidecars on disk.
/// - Sidecar without package: kept as `pending_json` until the package shows
///   up. Package without sidecar: imported so it shows in the library; it waits
///   for its sidecar. Package removed: dropped from the managed crates, the
///   track stays in the library.
/// - Beats/cues/key of a track loaded in a deck are deferred until the deck
///   unloads it (`slotDeckTrackChanged`); an unknown sample rate is probed on a
///   worker (`SoundSourceProxy::openAudioSource`) and the import completed.
/// - `<lib>/tracks` is registered once as a library root directory.
/// - Renamed files (same djlib track id, new file): `TrackDAO` relocation is not
///   done (core diff); the new file becomes a new library track and the djlib
///   state starts fresh on it (fingerprints of the old row would otherwise
///   read as DJ edits). DJ edits on the old row stay with that (missing) row.
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
        int waitingForPackage = 0; // sidecars without package
        int deferred = 0;          // waiting for a deck to unload / a sample rate
        int queued = 0;            // queue items not processed yet
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

    /// Tells whether a track is loaded in a deck/sampler (default: never).
    /// Wired to `PlayerInfo::isTrackLoaded` in CoreServices.
    void setTrackLoadedFn(TrackLoadedFn fn);

    /// Reads `[djlib] LibraryPath` and (re)starts watching.
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
            PackageRemoved,
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
    };

    struct Deferred {
        DjlibSidecar sidecar;
        TrackId trackId;
    };

    void rearm();
    void registerLibraryRoot();
    void watchDirectories();
    void slotDirectoryChanged();
    void startPass();
    void onPassFinished(djlib::PassResult result);
    void startMembershipJob(const QSet<QString>& crateIds);
    void startProbe(const TrackPointer& pTrack);

    void enqueue(Item item);
    void processNext();
    void process(const Item& item);
    void reconcile(const QList<djlib::CrateRef>& crates, bool membershipsIncluded);
    void packageRemoved(const QString& packageName);
    void packageAdded(const QString& packageName);
    void sidecarChanged(const djlib::ParsedSidecar& parsed);
    void applyMembership(const QString& djlibId, const QStringList& crateIds);
    void retry(const QString& djlibId);
    void applyToTrack(const TrackPointer& pTrack, const DjlibSidecar& sidecar);
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
    QElapsedTimer m_burst;
    QThreadPool m_pool;

    bool m_passRunning = false;
    bool m_passRequested = false;
    int m_jobsRunning = 0;
    bool m_reconcileQueued = false;

    QHash<QString, djlib::FileStamp> m_snapshot;
    std::optional<djlib::FileStamp> m_cratesStamp;
    std::optional<QList<djlib::CrateRef>> m_crates; // last valid crates.json

    QQueue<Item> m_queue;
    QHash<QString, Deferred> m_deferred; // djlib id -> sidecar
    QSet<TrackId> m_probing;
    QSet<TrackId> m_probed;

    QDateTime m_lastImport;
    QString m_lastImportFile;
};
