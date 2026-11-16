// Debug-only end-to-end test of MTP file operations against a real phone.
// Usage: MacDroid --selftest   (creates and removes "MacDroid-selftest" in Download)
#ifndef NDEBUG

#include "core/MtpWorker.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <cstdio>

namespace {

void log(const QString &s)
{
    std::fprintf(stderr, "[selftest] %s\n", qPrintable(s));
}

template <typename Signal, typename Pred>
bool waitFor(MtpWorker *w, Signal sig, Pred pred, int ms = 60000)
{
    QEventLoop loop;
    bool ok = false;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    auto c = QObject::connect(w, sig, &loop, [&](auto &&...args) {
        if (pred(args...)) {
            ok = true;
            loop.quit();
        }
    });
    loop.exec();
    QObject::disconnect(c);
    return ok;
}

} // namespace

int runSelfTest()
{
    MtpWorker w;
    w.start();
    mtp::DeviceInfo dev;
    w.rescan();
    if (!waitFor(&w, &MtpWorker::deviceOpened, [&](const mtp::DeviceInfo &i) { dev = i; return true; }, 15000)) {
        log("FAIL: no MTP device opened");
        return 1;
    }
    log(QStringLiteral("device %1, storages %2").arg(dev.name).arg(dev.storages.size()));
    const quint32 storage = dev.storages.first().id;

    auto list = [&](quint32 parent) {
        mtp::EntryList out;
        const quint64 r = w.listFolder(storage, parent);
        waitFor(&w, &MtpWorker::folderListed, [&](quint64 req, quint32, quint32, const mtp::EntryList &e, const QString &err) {
            if (req != r) return false;
            if (!err.isEmpty()) log("list error: " + err);
            out = e;
            return true;
        });
        return out;
    };
    auto find = [&](const mtp::EntryList &l, const QString &n) -> const mtp::Entry * {
        for (const auto &e : l) if (e.name == n) return &e;
        return nullptr;
    };
    auto op = [&](quint64 r, quint32 *newId = nullptr) {
        bool ok = false;
        waitFor(&w, &MtpWorker::operationFinished, [&](quint64 req, bool o, const QString &err, quint32 id) {
            if (req != r) return false;
            ok = o;
            if (!o) log("op error: " + err);
            if (newId) *newId = id;
            return true;
        });
        return ok;
    };
    auto job = [&](quint64 r) {
        bool ok = false;
        waitFor(&w, &MtpWorker::jobFinished, [&](quint64 req, bool o, bool, const QString &err) {
            if (req != r) return false;
            ok = o;
            if (!o) log("job error: " + err);
            return true;
        }, 120000);
        return ok;
    };
    int failures = 0;
    auto check = [&](bool cond, const QString &what) {
        log(QStringLiteral("%1  %2").arg(cond ? "PASS" : "FAIL", what));
        failures += !cond;
    };

    const mtp::EntryList root = list(mtp::kRootId);
    const mtp::Entry *download = find(root, "Download");
    check(download, "root listing contains Download");
    if (!download) return 1;
    const quint32 dl = download->id;

    // Clean leftovers from an earlier run.
    for (const auto &e : list(dl))
        if (e.name.startsWith("MacDroid-selftest"))
            job(w.remove({mtp::ItemRef{e.id, storage, dl, e.name, e.size, e.isFolder}}));

    quint32 folderId = 0;
    check(op(w.createFolder(storage, dl, "MacDroid-selftest"), &folderId) && folderId, "create folder");
    quint32 subId = 0;
    check(op(w.createFolder(storage, folderId, "sub"), &subId) && subId, "create subfolder");

    const QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/macdroid-selftest";
    QDir(tmp).removeRecursively();
    QDir().mkpath(tmp + "/up");
    QFile f(tmp + "/up/hello.txt");
    f.open(QIODevice::WriteOnly);
    f.write(QByteArray(200000, 'x'));
    f.close();
    QDir().mkpath(tmp + "/up/folder/inner");
    QFile f2(tmp + "/up/folder/inner/a.txt");
    f2.open(QIODevice::WriteOnly);
    f2.write("abc");
    f2.close();

    check(job(w.upload({tmp + "/up/hello.txt", tmp + "/up/folder"}, storage, folderId, mtp::ConflictPolicy::KeepBoth)), "upload file + folder");
    mtp::EntryList inFolder = list(folderId);
    const mtp::Entry *hello = find(inFolder, "hello.txt");
    check(hello && hello->size == 200000, "uploaded file present with size");
    check(find(inFolder, "folder"), "uploaded folder present");
    if (!hello) return 1;
    const mtp::ItemRef helloRef{hello->id, storage, folderId, hello->name, hello->size, false};

    check(job(w.copyOnDevice({helloRef}, storage, folderId)), "duplicate in same folder");
    check(find(list(folderId), "hello 2.txt"), "duplicate named 'hello 2.txt'");

    check(job(w.copyOnDevice({helloRef}, storage, subId)), "copy into subfolder");
    check(find(list(subId), "hello.txt"), "copy present in subfolder");

    check(op(w.rename(hello->id, "renamed.txt")), "rename");
    inFolder = list(folderId);
    const mtp::Entry *renamed = find(inFolder, "renamed.txt");
    check(renamed, "renamed file present");

    if (renamed) {
        const mtp::ItemRef r{renamed->id, storage, folderId, renamed->name, renamed->size, false};
        check(job(w.moveOnDevice({r}, storage, subId)), "move into subfolder");
        check(find(list(subId), "renamed.txt") && !find(list(folderId), "renamed.txt"), "moved file only in target");
    }

    const mtp::Entry *folderEntry = nullptr;
    inFolder = list(folderId);
    folderEntry = find(inFolder, "folder");
    QDir().mkpath(tmp + "/down");
    if (folderEntry) {
        const mtp::ItemRef fr{folderEntry->id, storage, folderId, "folder", 0, true};
        check(job(w.download({fr}, tmp + "/down", mtp::ConflictPolicy::Replace)), "download folder");
        check(QFile(tmp + "/down/folder/inner/a.txt").size() == 3, "downloaded nested file content");
    }

    const mtp::ItemRef top{folderId, storage, dl, "MacDroid-selftest", 0, true};
    check(job(w.remove({top})), "delete test folder recursively");
    check(!find(list(dl), "MacDroid-selftest"), "test folder gone");

    QDir(tmp).removeRecursively();
    log(QStringLiteral("done, %1 failure(s)").arg(failures));
    w.stop();
    return failures ? 2 : 0;
}

#endif
