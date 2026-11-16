#include "core/MtpWorker.h"

#include "platform/MacPlatform.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QProcess>
#include <QThread>
#include <QUuid>

#include <libmtp.h>

#include <cstdlib>
#include <cstring>

using namespace mtp;

namespace {

constexpr qint64 kMaxImageThumbSource = 40LL * 1000 * 1000;
constexpr qint64 kMaxPdfThumbSource = 80LL * 1000 * 1000;
constexpr qint64 kMaxVideoThumbSource = 250LL * 1000 * 1000;

// MTP: listing the root uses 0xFFFFFFFF, creating/moving into the root uses 0.
quint32 toTargetParent(quint32 parentId)
{
    return parentId == kRootId ? 0 : parentId;
}

QString cacheDir(const QString &sub)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + u'/' + sub;
    QDir().mkpath(dir);
    return dir;
}

QString fromC(const char *s)
{
    return s ? QString::fromUtf8(s) : QString();
}

} // namespace

// Shared progress state for one job (possibly many files).
struct MtpWorker::Progress {
    MtpWorker *worker = nullptr;
    quint64 jobId = 0;
    Priority priority = Priority::Transfer;
    qint64 totalBytes = 0;
    qint64 doneBytes = 0;     // bytes of finished files
    qint64 currentBytes = 0;  // bytes of the file in flight
    int totalItems = 0;
    int doneItems = 0;
    QString currentName;
    QElapsedTimer throttle;
    bool silent = false;      // background job: no progress signals
    bool preempted = false;

    void emitProgress(bool force = false)
    {
        if (silent)
            return;
        if (!force && throttle.isValid() && throttle.elapsed() < 80)
            return;
        throttle.restart();
        emit worker->jobProgress(jobId, doneBytes + currentBytes, totalBytes, currentName, doneItems, totalItems);
    }
};

int MtpWorker::progressThunk(uint64_t sent, uint64_t, void const *data)
{
    auto *pr = static_cast<Progress *>(const_cast<void *>(data));
    pr->currentBytes = qint64(sent);
    pr->emitProgress();
    if (pr->worker->isCancelled())
        return 1;
    if (pr->priority == Priority::Background && pr->worker->higherPriorityPending(pr->priority)) {
        pr->preempted = true;
        return 1;
    }
    return 0;
}

MtpWorker::MtpWorker(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<mtp::Entry>();
    qRegisterMetaType<mtp::EntryList>();
    qRegisterMetaType<mtp::DeviceInfo>();
    qRegisterMetaType<mtp::StorageList>();
    qRegisterMetaType<mtp::Path>();
}

MtpWorker::~MtpWorker()
{
    stop();
}

void MtpWorker::start()
{
    if (m_thread.joinable())
        return;
    LIBMTP_Init();
    m_stopping = false;
    m_thread = std::thread([this] { threadMain(); });
}

void MtpWorker::stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_queue.clear();
    }
    m_cancelCurrent = true;
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void MtpWorker::setConflictResolver(ConflictResolver resolver)
{
    std::lock_guard lock(m_mutex);
    m_resolver = std::move(resolver);
}

QString MtpWorker::deviceSerial() const
{
    std::lock_guard lock(m_infoMutex);
    return m_serial;
}

quint64 MtpWorker::enqueue(Priority priority, std::function<void(Job &)> fn)
{
    Job job;
    job.id = m_nextId++;
    job.priority = priority;
    job.run = std::move(fn);
    const quint64 id = job.id;
    {
        std::lock_guard lock(m_mutex);
        m_queue.emplace(std::make_pair(int(priority), id), std::move(job));
    }
    m_cv.notify_one();
    return id;
}

bool MtpWorker::higherPriorityPending(Priority p)
{
    std::lock_guard lock(m_mutex);
    return !m_queue.empty() && m_queue.begin()->first.first < int(p);
}

void MtpWorker::cancel(quint64 requestId)
{
    bool wasQueued = false;
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (it->second.id == requestId) {
                m_queue.erase(it);
                wasQueued = true;
                break;
            }
        }
    }
    if (wasQueued)
        emit jobFinished(requestId, false, true, QString());
    else if (m_currentId.load() == requestId)
        m_cancelCurrent = true;
}

void MtpWorker::cancelBackground()
{
    std::lock_guard lock(m_mutex);
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (it->first.first == int(Priority::Background))
            it = m_queue.erase(it);
        else
            ++it;
    }
}

void MtpWorker::threadMain()
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_stopping)
                break;
            auto it = m_queue.begin();
            job = std::move(it->second);
            m_queue.erase(it);
            m_currentId = job.id;
            m_cancelCurrent = false;
        }
        job.run(job);
        m_currentId = 0;
        if (job.requeue && !m_stopping) {
            job.requeue = false;
            std::lock_guard lock(m_mutex);
            // Put it back at the end of its priority band.
            m_queue.emplace(std::make_pair(int(job.priority), m_nextId++), std::move(job));
        }
    }
    closeDevice();
}

// ---------------------------------------------------------------- device

quint64 MtpWorker::rescan()
{
    return enqueue(Priority::Interactive, [this](Job &) { doRescan(); });
}

bool MtpWorker::imageCaptureRunning()
{
    return QProcess::execute(QStringLiteral("/usr/bin/pgrep"), {QStringLiteral("-x"), QStringLiteral("ptpcamerad")}) == 0;
}

quint64 MtpWorker::releaseFromImageCapture()
{
    return enqueue(Priority::Interactive, [this](Job &) {
        if (m_dev)
            return;
        // ptpcamerad is relaunched on demand, so stop it and open right away;
        // retry a few times in case it wins the race.
        for (int attempt = 0; attempt < 6; ++attempt) {
            QProcess::execute(QStringLiteral("/usr/bin/killall"), {QStringLiteral("ptpcamerad")});
            QThread::msleep(attempt == 0 ? 150 : 300);
            LIBMTP_raw_device_t *raw = nullptr;
            int count = 0;
            if (LIBMTP_Detect_Raw_Devices(&raw, &count) != LIBMTP_ERROR_NONE || count == 0) {
                std::free(raw);
                continue;
            }
            const bool ok = openDevice(&raw[0], false);
            std::free(raw);
            if (ok)
                return;
        }
        emit deviceHeldByImageCapture();
    });
}

quint64 MtpWorker::refreshStorages()
{
    return enqueue(Priority::Interactive, [this](Job &) {
        if (!m_dev)
            return;
        readStorages();
        emit storagesChanged(m_info.storages);
    });
}

void MtpWorker::doRescan()
{
    LIBMTP_raw_device_t *raw = nullptr;
    int count = 0;
    const LIBMTP_error_number_t err = LIBMTP_Detect_Raw_Devices(&raw, &count);

    if (m_dev) {
        bool stillThere = false;
        for (int i = 0; err == LIBMTP_ERROR_NONE && i < count; ++i) {
            if (raw[i].bus_location == m_busLocation && raw[i].devnum == m_devNum)
                stillThere = true;
        }
        std::free(raw);
        if (!stillThere) {
            closeDevice();
            emit deviceClosed();
            return;
        }
        // Device is open: refresh storages (they appear once the phone is unlocked).
        readStorages();
        if (m_info.storages.isEmpty()) {
            m_reportedReady = false;
            emit deviceNotReady(tr("Unlock your phone to allow access to its files."), true);
        } else if (!m_reportedReady) {
            m_reportedReady = true;
            emit deviceOpened(m_info);
        } else {
            emit storagesChanged(m_info.storages);
        }
        return;
    }

    if (err == LIBMTP_ERROR_NO_DEVICE_ATTACHED || count == 0) {
        std::free(raw);
        emit noDevice();
        return;
    }
    if (err != LIBMTP_ERROR_NONE) {
        std::free(raw);
        emit deviceNotReady(tr("The phone could not be read over USB."), false);
        return;
    }
    openDevice(&raw[0]);
    std::free(raw);
}

bool MtpWorker::openDevice(void *rawPtr, bool reportFailure)
{
    auto *raw = static_cast<LIBMTP_raw_device_t *>(rawPtr);
    m_dev = LIBMTP_Open_Raw_Device_Uncached(raw);
    if (!m_dev) {
        qInfo("MacDroid: could not open MTP device %04x:%04x", raw->device_entry.vendor_id, raw->device_entry.product_id);
        if (!reportFailure)
            return false;
        if (imageCaptureRunning())
            emit deviceHeldByImageCapture();
        else
            emit deviceNotReady(tr("Another app may be using the phone. Quit Android File Transfer, "
                                   "OpenMTP or Image Capture, then try again."), false);
        return false;
    }
    m_busLocation = raw->bus_location;
    m_devNum = raw->devnum;

    DeviceInfo info;
    char *s = LIBMTP_Get_Friendlyname(m_dev);
    info.name = fromC(s);
    std::free(s);
    s = LIBMTP_Get_Manufacturername(m_dev);
    info.manufacturer = fromC(s).trimmed();
    std::free(s);
    s = LIBMTP_Get_Modelname(m_dev);
    info.model = fromC(s).trimmed();
    std::free(s);
    s = LIBMTP_Get_Serialnumber(m_dev);
    info.serial = fromC(s);
    std::free(s);
    if (info.name.isEmpty())
        info.name = info.model.isEmpty() ? fromC(raw->device_entry.product) : info.model;
    if (info.name.isEmpty())
        info.name = tr("Android Phone");
    uint8_t maxLevel = 0, curLevel = 0;
    if (LIBMTP_Get_Batterylevel(m_dev, &maxLevel, &curLevel) == 0 && maxLevel > 0)
        info.batteryPercent = int(curLevel) * 100 / int(maxLevel);
    LIBMTP_Clear_Errorstack(m_dev);

    m_info = info;
    {
        std::lock_guard lock(m_infoMutex);
        m_serial = info.serial;
    }
    readStorages();
    if (m_info.storages.isEmpty()) {
        m_reportedReady = false;
        emit deviceNotReady(tr("Unlock your phone to allow access to its files."), true);
    } else {
        m_reportedReady = true;
        emit deviceOpened(m_info);
    }
    return true;
}

void MtpWorker::closeDevice()
{
    if (!m_dev)
        return;
    LIBMTP_Release_Device(m_dev);
    m_dev = nullptr;
    m_info = {};
    m_reportedReady = false;
    {
        std::lock_guard lock(m_infoMutex);
        m_serial.clear();
    }
    // Everything still queued refers to the old session.
    std::vector<quint64> dropped;
    {
        std::lock_guard lock(m_mutex);
        for (auto &[key, job] : m_queue)
            dropped.push_back(job.id);
        m_queue.clear();
    }
    for (quint64 id : dropped)
        emit jobFinished(id, false, true, QString());
}

void MtpWorker::readStorages()
{
    m_info.storages.clear();
    if (!m_dev)
        return;
    if (LIBMTP_Get_Storage(m_dev, LIBMTP_STORAGE_SORTBY_NOTSORTED) != 0) {
        LIBMTP_Clear_Errorstack(m_dev);
        return;
    }
    for (LIBMTP_devicestorage_t *st = m_dev->storage; st; st = st->next) {
        Storage s;
        s.id = st->id;
        s.description = fromC(st->StorageDescription);
        s.capacity = qint64(st->MaxCapacity);
        s.freeBytes = qint64(st->FreeSpaceInBytes);
        s.removable = st->StorageType == 0x0002 || st->StorageType == 0x0004; // removable ROM/RAM
        if (s.description.isEmpty())
            s.description = s.removable ? tr("SD card") : tr("Internal storage");
        m_info.storages.append(s);
    }
}

QString MtpWorker::lastError(const QString &fallback)
{
    if (!m_dev)
        return tr("The phone was disconnected.");
    QString text;
    for (LIBMTP_error_t *e = LIBMTP_Get_Errorstack(m_dev); e; e = e->next) {
        if (e->error_text) {
            text = QString::fromUtf8(e->error_text).trimmed();
            break;
        }
    }
    LIBMTP_Clear_Errorstack(m_dev);
    return text.isEmpty() ? fallback : fallback + u'\n' + text;
}

EntryList MtpWorker::listChildren(quint32 storageId, quint32 parentId, bool *ok)
{
    EntryList out;
    if (ok)
        *ok = m_dev != nullptr;
    if (!m_dev)
        return out;
    LIBMTP_file_t *files = LIBMTP_Get_Files_And_Folders(m_dev, storageId, parentId);
    for (LIBMTP_file_t *f = files; f;) {
        Entry e;
        e.id = f->item_id;
        e.parentId = f->parent_id;
        e.storageId = f->storage_id;
        e.name = fromC(f->filename);
        e.size = qint64(f->filesize);
        e.modified = QDateTime::fromSecsSinceEpoch(qint64(f->modificationdate));
        e.isFolder = f->filetype == LIBMTP_FILETYPE_FOLDER;
        if (!e.name.isEmpty())
            out.append(e);
        LIBMTP_file_t *next = f->next;
        LIBMTP_destroy_file_t(f);
        f = next;
    }
    if (!files && ok) {
        // An empty folder also returns NULL; only treat a populated error stack as failure.
        if (LIBMTP_Get_Errorstack(m_dev))
            *ok = false;
    }
    LIBMTP_Clear_Errorstack(m_dev);
    return out;
}

// ---------------------------------------------------------------- browsing

quint64 MtpWorker::listFolder(quint32 storageId, quint32 parentId)
{
    return enqueue(Priority::Interactive, [this, storageId, parentId](Job &job) {
        bool ok = false;
        const EntryList list = listChildren(storageId, parentId, &ok);
        emit folderListed(job.id, storageId, parentId, list, ok ? QString() : tr("Could not read this folder."));
    });
}

quint64 MtpWorker::resolvePath(quint32 storageId, const QStringList &parts)
{
    return enqueue(Priority::Interactive, [this, storageId, parts](Job &job) {
        Path path;
        path.append(PathNode{kRootId, QString()});
        quint32 parent = kRootId;
        for (const QString &part : parts) {
            const EntryList children = listChildren(storageId, parent);
            const Entry *found = nullptr;
            for (const Entry &e : children) {
                if (e.isFolder && e.name.compare(part, Qt::CaseInsensitive) == 0) {
                    found = &e;
                    break;
                }
            }
            if (!found) {
                emit pathResolved(job.id, storageId, path, tr("The folder \"%1\" was not found.").arg(part));
                return;
            }
            parent = found->id;
            path.append(PathNode{found->id, found->name});
        }
        emit pathResolved(job.id, storageId, path, QString());
    });
}

quint64 MtpWorker::createFolder(quint32 storageId, quint32 parentId, const QString &name)
{
    return enqueue(Priority::Interactive, [this, storageId, parentId, name](Job &job) {
        if (!m_dev) {
            emit operationFinished(job.id, false, tr("The phone was disconnected."), 0);
            return;
        }
        QByteArray utf8 = name.toUtf8();
        const quint32 id = LIBMTP_Create_Folder(m_dev, utf8.data(), toTargetParent(parentId), storageId);
        const bool ok = id != 0;
        emit operationFinished(job.id, ok, ok ? QString() : lastError(tr("Could not create the folder.")), id);
        if (ok)
            emit folderChanged(storageId, parentId);
    });
}

quint64 MtpWorker::rename(quint32 objectId, const QString &newName)
{
    return enqueue(Priority::Interactive, [this, objectId, newName](Job &job) {
        if (!m_dev) {
            emit operationFinished(job.id, false, tr("The phone was disconnected."), 0);
            return;
        }
        QByteArray utf8 = newName.toUtf8();
        const bool ok = LIBMTP_Set_Object_Filename(m_dev, objectId, utf8.data()) == 0;
        emit operationFinished(job.id, ok, ok ? QString() : lastError(tr("Could not rename the item.")), objectId);
    });
}

bool MtpWorker::deleteObject(quint32 storageId, quint32 id, bool isFolder)
{
    if (!m_dev)
        return false;
    if (LIBMTP_Delete_Object(m_dev, id) == 0)
        return true;
    LIBMTP_Clear_Errorstack(m_dev);
    if (!isFolder)
        return false;
    // Some devices refuse to delete non-empty folders: delete depth first.
    for (const Entry &child : listChildren(storageId, id)) {
        if (isCancelled() || !deleteObject(storageId, child.id, child.isFolder))
            return false;
    }
    return LIBMTP_Delete_Object(m_dev, id) == 0;
}

quint64 MtpWorker::remove(const ItemRefList &items)
{
    return enqueue(Priority::Interactive, [this, items](Job &job) {
        Progress pr;
        pr.worker = this;
        pr.jobId = job.id;
        pr.totalItems = int(items.size());
        const QString title = items.size() == 1 ? tr("Deleting \"%1\"").arg(items.first().name)
                                                : tr("Deleting %n items", nullptr, int(items.size()));
        emit jobStarted(job.id, title, int(JobKind::Delete));
        QSet<QPair<quint32, quint32>> touched;
        for (const ItemRef &it : items) {
            if (isCancelled())
                break;
            pr.currentName = it.name;
            pr.emitProgress(true);
            if (!deleteObject(it.storageId, it.id, it.isFolder)) {
                finishJob(job, pr, false, lastError(tr("Could not delete \"%1\".").arg(it.name)));
                emit folderChanged(it.storageId, it.parentId);
                return;
            }
            touched.insert({it.storageId, it.parentId});
            ++pr.doneItems;
        }
        for (const auto &t : touched)
            emit folderChanged(t.first, t.second);
        finishJob(job, pr, !isCancelled(), QString());
    });
}

void MtpWorker::finishJob(Job &job, Progress &pr, bool ok, const QString &error)
{
    const bool cancelled = isCancelled();
    pr.currentBytes = 0;
    pr.emitProgress(true);
    emit jobFinished(job.id, ok && !cancelled, cancelled, cancelled ? QString() : error);
    if (m_dev)
        readStorages(), emit storagesChanged(m_info.storages);
}

// ---------------------------------------------------------------- transfers

qint64 MtpWorker::deviceTreeSize(const ItemRef &item, int *count)
{
    if (!item.isFolder) {
        ++*count;
        return item.size;
    }
    qint64 total = 0;
    for (const Entry &e : listChildren(item.storageId, item.id)) {
        if (isCancelled())
            break;
        total += deviceTreeSize(ItemRef{e.id, e.storageId, e.parentId, e.name, e.size, e.isFolder}, count);
    }
    return total;
}

bool MtpWorker::downloadTree(const ItemRef &item, const QString &localPath, Progress &pr)
{
    if (isCancelled() || !m_dev)
        return false;
    if (item.isFolder) {
        if (!QDir().mkpath(localPath))
            return false;
        for (const Entry &e : listChildren(item.storageId, item.id)) {
            if (!downloadTree(ItemRef{e.id, e.storageId, e.parentId, e.name, e.size, e.isFolder},
                              localPath + u'/' + e.name, pr))
                return false;
        }
        return true;
    }
    pr.currentName = item.name;
    pr.currentBytes = 0;
    const QByteArray path = QFile::encodeName(localPath);
    if (LIBMTP_Get_File_To_File(m_dev, item.id, path.constData(), &MtpWorker::progressThunk, &pr) != 0) {
        QFile::remove(localPath);
        return false;
    }
    pr.doneBytes += item.size;
    pr.currentBytes = 0;
    ++pr.doneItems;
    pr.emitProgress(true);
    return true;
}

bool MtpWorker::uploadTree(const QString &localPath, quint32 storageId, quint32 parentId,
                           const QString &targetName, Progress &pr, quint32 *newId)
{
    if (isCancelled() || !m_dev)
        return false;
    const QFileInfo fi(localPath);
    if (fi.isDir()) {
        QByteArray utf8 = targetName.toUtf8();
        const quint32 folderId = LIBMTP_Create_Folder(m_dev, utf8.data(), toTargetParent(parentId), storageId);
        if (folderId == 0)
            return false;
        if (newId)
            *newId = folderId;
        const QFileInfoList children = QDir(localPath).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo &c : children) {
            if (c.fileName() == QLatin1String(".DS_Store"))
                continue;
            if (!uploadTree(c.absoluteFilePath(), storageId, folderId, c.fileName(), pr))
                return false;
        }
        return true;
    }

    LIBMTP_file_t *file = LIBMTP_new_file_t();
    file->filename = strdup(targetName.toUtf8().constData());
    file->filesize = uint64_t(fi.size());
    file->filetype = LIBMTP_FILETYPE_UNKNOWN;
    file->parent_id = toTargetParent(parentId);
    file->storage_id = storageId;
    file->modificationdate = time_t(fi.lastModified().toSecsSinceEpoch());
    pr.currentName = targetName;
    pr.currentBytes = 0;
    const QByteArray path = QFile::encodeName(localPath);
    const int rc = LIBMTP_Send_File_From_File(m_dev, path.constData(), file, &MtpWorker::progressThunk, &pr);
    if (rc == 0 && newId)
        *newId = file->item_id;
    LIBMTP_destroy_file_t(file);
    if (rc != 0)
        return false;
    pr.doneBytes += fi.size();
    pr.currentBytes = 0;
    ++pr.doneItems;
    pr.emitProgress(true);
    return true;
}

static qint64 localTreeSize(const QString &path, int *count)
{
    const QFileInfo fi(path);
    if (!fi.isDir()) {
        ++*count;
        return fi.size();
    }
    qint64 total = 0;
    for (const QFileInfo &c : QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (c.fileName() != QLatin1String(".DS_Store"))
            total += localTreeSize(c.absoluteFilePath(), count);
    }
    return total;
}

quint64 MtpWorker::upload(const QStringList &localPaths, quint32 storageId, quint32 parentId, ConflictPolicy policy)
{
    return enqueue(Priority::Transfer, [this, localPaths, storageId, parentId, policy](Job &job) {
        Progress pr;
        pr.worker = this;
        pr.jobId = job.id;
        const QString title = localPaths.size() == 1
            ? tr("Copying \"%1\" to phone").arg(QFileInfo(localPaths.first()).fileName())
            : tr("Copying %n items to phone", nullptr, int(localPaths.size()));
        emit jobStarted(job.id, title, int(JobKind::Upload));
        for (const QString &p : localPaths)
            pr.totalBytes += localTreeSize(p, &pr.totalItems);
        pr.emitProgress(true);

        EntryList existing = listChildren(storageId, parentId);
        auto exists = [&existing](const QString &n) {
            for (const Entry &e : existing)
                if (e.name.compare(n, Qt::CaseInsensitive) == 0)
                    return true;
            return false;
        };
        ConflictPolicy sticky = policy;
        bool ok = true;
        QString error;
        for (const QString &p : localPaths) {
            if (isCancelled())
                break;
            const QFileInfo fi(p);
            QString name = fi.fileName();
            if (exists(name)) {
                ConflictPolicy choice = sticky;
                if (choice == ConflictPolicy::Ask) {
                    bool all = false;
                    ConflictResolver resolver;
                    {
                        std::lock_guard lock(m_mutex);
                        resolver = m_resolver;
                    }
                    choice = resolver ? resolver(name, fi.isDir(), &all) : ConflictPolicy::KeepBoth;
                    if (all)
                        sticky = choice;
                }
                if (choice == ConflictPolicy::Stop)
                    break;
                if (choice == ConflictPolicy::Skip) {
                    int n = 0;
                    pr.doneBytes += localTreeSize(p, &n);
                    pr.doneItems += n;
                    continue;
                }
                if (choice == ConflictPolicy::KeepBoth) {
                    name = uniqueName(name, exists);
                } else { // Replace
                    for (const Entry &e : existing) {
                        if (e.name.compare(name, Qt::CaseInsensitive) == 0) {
                            deleteObject(storageId, e.id, e.isFolder);
                            break;
                        }
                    }
                }
            }
            if (!uploadTree(p, storageId, parentId, name, pr)) {
                if (!isCancelled()) {
                    ok = false;
                    error = lastError(tr("Could not copy \"%1\" to the phone.").arg(fi.fileName()));
                }
                break;
            }
            Entry added;
            added.name = name;
            existing.append(added);
        }
        emit folderChanged(storageId, parentId);
        finishJob(job, pr, ok, error);
    });
}

quint64 MtpWorker::download(const ItemRefList &items, const QString &localDir, ConflictPolicy policy,
                            Completion done, const QString &titleOverride)
{
    return enqueue(Priority::Transfer, [this, items, localDir, policy, done, titleOverride](Job &job) {
        Progress pr;
        pr.worker = this;
        pr.jobId = job.id;
        const QString title = !titleOverride.isEmpty() ? titleOverride
            : items.size() == 1 ? tr("Copying \"%1\" to Mac").arg(items.first().name)
                                : tr("Copying %n items to Mac", nullptr, int(items.size()));
        emit jobStarted(job.id, title, int(JobKind::Download));
        for (const ItemRef &it : items)
            pr.totalBytes += deviceTreeSize(it, &pr.totalItems);
        pr.emitProgress(true);

        QDir().mkpath(localDir);
        ConflictPolicy sticky = policy;
        bool ok = true;
        QString error;
        for (const ItemRef &it : items) {
            if (isCancelled())
                break;
            QString name = it.name;
            QString target = localDir + u'/' + name;
            if (QFileInfo::exists(target)) {
                ConflictPolicy choice = sticky;
                if (choice == ConflictPolicy::Ask) {
                    bool all = false;
                    ConflictResolver resolver;
                    {
                        std::lock_guard lock(m_mutex);
                        resolver = m_resolver;
                    }
                    choice = resolver ? resolver(name, it.isFolder, &all) : ConflictPolicy::KeepBoth;
                    if (all)
                        sticky = choice;
                }
                if (choice == ConflictPolicy::Stop)
                    break;
                if (choice == ConflictPolicy::Skip) {
                    int n = 0;
                    pr.doneBytes += deviceTreeSize(it, &n);
                    pr.doneItems += n;
                    continue;
                }
                if (choice == ConflictPolicy::KeepBoth) {
                    name = uniqueName(name, [&localDir](const QString &n) { return QFileInfo::exists(localDir + u'/' + n); });
                    target = localDir + u'/' + name;
                } else {
                    QFileInfo(target).isDir() ? (void)QDir(target).removeRecursively() : (void)QFile::remove(target);
                }
            }
            if (!downloadTree(it, target, pr)) {
                if (!isCancelled()) {
                    ok = false;
                    error = lastError(tr("Could not copy \"%1\" to the Mac.").arg(it.name));
                }
                break;
            }
        }
        const bool success = ok && !isCancelled();
        finishJob(job, pr, ok, error);
        if (done)
            done(success);
    });
}

quint64 MtpWorker::moveOnDevice(const ItemRefList &items, quint32 storageId, quint32 parentId)
{
    return enqueue(Priority::Transfer, [this, items, storageId, parentId](Job &job) {
        Progress pr;
        pr.worker = this;
        pr.jobId = job.id;
        pr.totalItems = int(items.size());
        const QString title = items.size() == 1 ? tr("Moving \"%1\"").arg(items.first().name)
                                                : tr("Moving %n items", nullptr, int(items.size()));
        emit jobStarted(job.id, title, int(JobKind::MoveOnDevice));
        EntryList existing = listChildren(storageId, parentId);
        auto exists = [&existing](const QString &n) {
            for (const Entry &e : existing)
                if (e.name.compare(n, Qt::CaseInsensitive) == 0)
                    return true;
            return false;
        };
        QSet<QPair<quint32, quint32>> touched{{storageId, parentId}};
        bool ok = true;
        QString error;
        for (const ItemRef &it : items) {
            if (isCancelled())
                break;
            if (it.parentId == parentId && it.storageId == storageId) {
                ++pr.doneItems;
                continue;
            }
            if (it.id == parentId)
                continue; // can't move a folder into itself
            pr.currentName = it.name;
            pr.emitProgress(true);
            touched.insert({it.storageId, it.parentId});
            QString name = it.name;
            if (exists(name)) {
                // Never overwrite silently on move: keep both.
                name = uniqueName(name, exists);
                QByteArray utf8 = name.toUtf8();
                LIBMTP_Set_Object_Filename(m_dev, it.id, utf8.data());
            }
            if (m_dev && LIBMTP_Move_Object(m_dev, it.id, storageId, toTargetParent(parentId)) == 0) {
                ++pr.doneItems;
                Entry e;
                e.name = name;
                existing.append(e);
                continue;
            }
            if (m_dev)
                LIBMTP_Clear_Errorstack(m_dev);
            // Fallback: copy through a temporary folder on the Mac, then delete.
            const QString tmp = cacheDir(QStringLiteral("move/") + QUuid::createUuid().toString(QUuid::WithoutBraces));
            ItemRef src = it;
            src.name = name;
            int n = 0;
            pr.totalBytes += deviceTreeSize(src, &n);
            const bool copied = downloadTree(src, tmp + u'/' + name, pr)
                && uploadTree(tmp + u'/' + name, storageId, parentId, name, pr);
            QDir(tmp).removeRecursively();
            if (!copied || !deleteObject(it.storageId, it.id, it.isFolder)) {
                if (!isCancelled()) {
                    ok = false;
                    error = lastError(tr("Could not move \"%1\".").arg(it.name));
                }
                break;
            }
            Entry e;
            e.name = name;
            existing.append(e);
        }
        for (const auto &t : touched)
            emit folderChanged(t.first, t.second);
        finishJob(job, pr, ok, error);
    });
}

quint64 MtpWorker::copyOnDevice(const ItemRefList &items, quint32 storageId, quint32 parentId)
{
    return enqueue(Priority::Transfer, [this, items, storageId, parentId](Job &job) {
        Progress pr;
        pr.worker = this;
        pr.jobId = job.id;
        const QString title = items.size() == 1 ? tr("Copying \"%1\"").arg(items.first().name)
                                                : tr("Copying %n items", nullptr, int(items.size()));
        emit jobStarted(job.id, title, int(JobKind::CopyOnDevice));
        for (const ItemRef &it : items)
            pr.totalBytes += deviceTreeSize(it, &pr.totalItems) * 2; // down + up
        pr.emitProgress(true);

        EntryList existing = listChildren(storageId, parentId);
        auto exists = [&existing](const QString &n) {
            for (const Entry &e : existing)
                if (e.name.compare(n, Qt::CaseInsensitive) == 0)
                    return true;
            return false;
        };
        bool ok = true;
        QString error;
        for (const ItemRef &it : items) {
            if (isCancelled())
                break;
            const QString name = uniqueName(it.name, exists); // copies never replace: "name 2"
            // Native CopyObject keeps the same name, so only use it when there is no clash.
            if (name == it.name && !it.isFolder && m_dev
                && LIBMTP_Copy_Object(m_dev, it.id, storageId, toTargetParent(parentId)) == 0) {
                pr.doneBytes += it.size * 2;
                ++pr.doneItems;
            } else {
                if (m_dev)
                    LIBMTP_Clear_Errorstack(m_dev);
                const QString tmp = cacheDir(QStringLiteral("copy/") + QUuid::createUuid().toString(QUuid::WithoutBraces));
                const bool copied = downloadTree(it, tmp + u'/' + it.name, pr)
                    && uploadTree(tmp + u'/' + it.name, storageId, parentId, name, pr);
                QDir(tmp).removeRecursively();
                if (!copied) {
                    if (!isCancelled()) {
                        ok = false;
                        error = lastError(tr("Could not copy \"%1\".").arg(it.name));
                    }
                    break;
                }
            }
            Entry e;
            e.name = name;
            existing.append(e);
        }
        emit folderChanged(storageId, parentId);
        finishJob(job, pr, ok, error);
    });
}

// ---------------------------------------------------------------- thumbnails

quint64 MtpWorker::thumbnail(const Entry &entry, int pixelSize)
{
    return enqueue(Priority::Background, [this, entry, pixelSize](Job &job) {
        if (!m_dev)
            return;
        const QString dir = cacheDir(QStringLiteral("thumbs/") + (m_info.serial.isEmpty() ? QStringLiteral("device") : m_info.serial));
        const QString cachePath = QStringLiteral("%1/%2_%3_%4_%5.png")
                                      .arg(dir).arg(entry.id).arg(entry.modified.toSecsSinceEpoch()).arg(entry.size).arg(pixelSize);
        QImage img;
        if (img.load(cachePath)) {
            emit thumbnailReady(entry.id, img);
            return;
        }

        if (entry.isImage()) {
            unsigned char *data = nullptr;
            unsigned int size = 0;
            if (LIBMTP_Get_Thumbnail(m_dev, entry.id, &data, &size) == 0 && data) {
                img.loadFromData(data, int(size));
                std::free(data);
            }
            LIBMTP_Clear_Errorstack(m_dev);
            // Embedded MTP thumbnails can be tiny; accept them only if reasonably sized.
            if (!img.isNull() && qMax(img.width(), img.height()) < pixelSize / 2)
                img = QImage();
        }

        if (img.isNull()) {
            const qint64 limit = entry.isVideo() ? kMaxVideoThumbSource
                               : entry.isPdf()   ? kMaxPdfThumbSource
                                                 : kMaxImageThumbSource;
            if (entry.size <= 0 || entry.size > limit)
                return;
            const QString tmp = cacheDir(QStringLiteral("thumbsrc")) + u'/' + QString::number(entry.id) + u'.' + entry.extension();
            Progress pr;
            pr.worker = this;
            pr.jobId = job.id;
            pr.priority = Priority::Background;
            pr.silent = true;
            const QByteArray path = QFile::encodeName(tmp);
            const int rc = LIBMTP_Get_File_To_File(m_dev, entry.id, path.constData(), &MtpWorker::progressThunk, &pr);
            LIBMTP_Clear_Errorstack(m_dev);
            if (rc != 0) {
                QFile::remove(tmp);
                if (pr.preempted)
                    job.requeue = true; // try again once user work is done
                return;
            }
            img = MacPlatform::quickLookThumbnail(tmp, pixelSize);
            QFile::remove(tmp);
        }
        if (img.isNull())
            return;
        if (qMax(img.width(), img.height()) > pixelSize)
            img = img.scaled(pixelSize, pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        img.save(cachePath, "PNG");
        emit thumbnailReady(entry.id, img);
    });
}
