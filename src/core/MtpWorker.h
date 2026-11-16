#pragma once

#include "core/MtpTypes.h"

#include <QImage>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

struct LIBMTP_mtpdevice_struct;

// Owns the single MTP session and serialises every device operation on one
// dedicated thread (libmtp is not thread safe). Requests are queued with a
// priority so that browsing stays responsive and thumbnails never delay user
// work. All public methods are thread safe and return a request id; results
// arrive through the signals (queued to the receiver's thread).
class MtpWorker : public QObject
{
    Q_OBJECT
public:
    enum class Priority { Interactive = 0, Transfer = 1, Background = 2 };
    enum class JobKind { Upload, Download, CopyOnDevice, MoveOnDevice, Delete };
    Q_ENUM(JobKind)

    // Called on the worker thread when a top-level item already exists at
    // the destination. Must return Replace, KeepBoth, Skip or Stop.
    using ConflictResolver = std::function<mtp::ConflictPolicy(const QString &name, bool isFolder, bool *applyToAll)>;
    // Called on the worker thread when a job ends (used by blocking callers
    // such as file promises).
    using Completion = std::function<void(bool ok)>;

    explicit MtpWorker(QObject *parent = nullptr);
    ~MtpWorker() override;

    void start();
    void stop();

    quint64 rescan();
    quint64 refreshStorages();
    // macOS' Image Capture daemon (ptpcamerad) grabs MTP phones. On the user's
    // request: stop it and claim the phone before it is relaunched.
    quint64 releaseFromImageCapture();
    quint64 listFolder(quint32 storageId, quint32 parentId);
    quint64 resolvePath(quint32 storageId, const QStringList &parts);
    quint64 createFolder(quint32 storageId, quint32 parentId, const QString &name);
    quint64 rename(quint32 objectId, const QString &newName);
    quint64 remove(const mtp::ItemRefList &items);
    quint64 upload(const QStringList &localPaths, quint32 storageId, quint32 parentId,
                   mtp::ConflictPolicy policy = mtp::ConflictPolicy::Ask);
    quint64 download(const mtp::ItemRefList &items, const QString &localDir,
                     mtp::ConflictPolicy policy = mtp::ConflictPolicy::Ask,
                     Completion done = {}, const QString &titleOverride = {});
    quint64 moveOnDevice(const mtp::ItemRefList &items, quint32 storageId, quint32 parentId);
    quint64 copyOnDevice(const mtp::ItemRefList &items, quint32 storageId, quint32 parentId);
    quint64 thumbnail(const mtp::Entry &entry, int pixelSize);

    void cancel(quint64 requestId);
    void cancelBackground();

    void setConflictResolver(ConflictResolver resolver);
    QString deviceSerial() const;

signals:
    void deviceOpened(const mtp::DeviceInfo &info);
    void deviceClosed();
    void deviceNotReady(const QString &reason, bool locked);
    // The phone is in MTP mode but held by Image Capture (ptpcamerad).
    void deviceHeldByImageCapture();
    void noDevice();
    void storagesChanged(const mtp::StorageList &storages);

    void folderListed(quint64 request, quint32 storageId, quint32 parentId,
                      const mtp::EntryList &entries, const QString &error);
    void pathResolved(quint64 request, quint32 storageId, const mtp::Path &path, const QString &error);
    void operationFinished(quint64 request, bool ok, const QString &error, quint32 newObjectId);

    void jobStarted(quint64 request, const QString &title, int kind);
    void jobProgress(quint64 request, qint64 doneBytes, qint64 totalBytes,
                     const QString &currentName, int doneItems, int totalItems);
    void jobFinished(quint64 request, bool ok, bool cancelled, const QString &error);
    // Emitted after any job that changed the content of a device folder.
    void folderChanged(quint32 storageId, quint32 parentId);

    void thumbnailReady(quint32 objectId, const QImage &image);

private:
    struct Job {
        quint64 id = 0;
        Priority priority = Priority::Interactive;
        std::function<void(Job &)> run;
        bool requeue = false;   // set by a preempted background job
    };
    struct Progress;

    quint64 enqueue(Priority priority, std::function<void(Job &)> fn);
    void threadMain();
    bool higherPriorityPending(Priority p);
    bool isCancelled() const { return m_cancelCurrent.load(); }

    // Everything below runs on the worker thread only.
    void doRescan();
    bool openDevice(void *rawDevice, bool reportFailure = true);
    void closeDevice();
    static bool imageCaptureRunning();
    void readStorages();
    QString lastError(const QString &fallback);
    mtp::EntryList listChildren(quint32 storageId, quint32 parentId, bool *ok = nullptr);
    bool deleteObject(quint32 storageId, quint32 id, bool isFolder);
    bool downloadTree(const mtp::ItemRef &item, const QString &localPath, Progress &pr);
    bool uploadTree(const QString &localPath, quint32 storageId, quint32 parentId,
                    const QString &targetName, Progress &pr, quint32 *newId = nullptr);
    qint64 deviceTreeSize(const mtp::ItemRef &item, int *count);
    void finishJob(Job &job, Progress &pr, bool ok, const QString &error);

    static int progressThunk(uint64_t sent, uint64_t total, void const *data);

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::multimap<std::pair<int, quint64>, Job> m_queue;
    bool m_stopping = false;
    std::atomic<quint64> m_nextId{1};
    std::atomic<quint64> m_currentId{0};
    std::atomic<bool> m_cancelCurrent{false};

    ConflictResolver m_resolver;
    mutable std::mutex m_infoMutex;
    QString m_serial;

    // Worker-thread state
    LIBMTP_mtpdevice_struct *m_dev = nullptr;
    quint32 m_busLocation = 0;
    quint8 m_devNum = 0;
    mtp::DeviceInfo m_info;
    bool m_reportedReady = false;
};
