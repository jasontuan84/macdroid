#include "core/TransferManager.h"

#include "core/MtpWorker.h"

qint64 TransferManager::Transfer::secondsLeft() const
{
    if (bytesPerSecond <= 1 || totalBytes <= doneBytes)
        return -1;
    return qint64(double(totalBytes - doneBytes) / bytesPerSecond);
}

TransferManager::TransferManager(MtpWorker *worker, QObject *parent)
    : QObject(parent)
    , m_worker(worker)
{
    connect(worker, &MtpWorker::jobStarted, this, [this](quint64 id, const QString &title, int kind) {
        Transfer t;
        t.id = id;
        t.title = title;
        t.kind = kind;
        t.clock.start();
        m_items.prepend(t);
        emit changed();
    });
    connect(worker, &MtpWorker::jobProgress, this,
            [this](quint64 id, qint64 done, qint64 total, const QString &name, int doneItems, int totalItems) {
        Transfer *t = find(id);
        if (!t)
            return;
        const qint64 now = t->clock.elapsed();
        const qint64 dt = now - t->lastMs;
        if (dt >= 250 && done >= t->lastBytes) {
            const double instant = double(done - t->lastBytes) * 1000.0 / double(dt);
            t->bytesPerSecond = t->bytesPerSecond <= 0 ? instant : t->bytesPerSecond * 0.7 + instant * 0.3;
            t->lastBytes = done;
            t->lastMs = now;
        }
        t->doneBytes = done;
        t->totalBytes = total;
        t->currentName = name;
        t->doneItems = doneItems;
        t->totalItems = totalItems;
        emit changed();
    });
    connect(worker, &MtpWorker::jobFinished, this, [this](quint64 id, bool ok, bool cancelled, const QString &error) {
        Transfer *t = find(id);
        if (!t)
            return;
        t->state = cancelled ? State::Cancelled : (ok ? State::Done : State::Failed);
        t->error = error;
        if (ok)
            t->doneBytes = t->totalBytes;
        emit finished(id, ok, cancelled, t->title, error);
        emit changed();
    });
}

TransferManager::Transfer *TransferManager::find(quint64 id)
{
    for (Transfer &t : m_items)
        if (t.id == id)
            return &t;
    return nullptr;
}

int TransferManager::activeCount() const
{
    int n = 0;
    for (const Transfer &t : m_items)
        n += t.state == State::Running;
    return n;
}

double TransferManager::overallFraction() const
{
    qint64 done = 0, total = 0;
    for (const Transfer &t : m_items) {
        if (t.state != State::Running)
            continue;
        done += t.doneBytes;
        total += t.totalBytes;
    }
    return total > 0 ? double(done) / double(total) : 0.0;
}

void TransferManager::clearFinished()
{
    m_items.removeIf([](const Transfer &t) { return t.state != State::Running; });
    emit changed();
}

void TransferManager::cancel(quint64 id)
{
    m_worker->cancel(id);
}
