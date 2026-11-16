#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QVector>

class MtpWorker;

// UI-thread model of running and finished transfer jobs (fed by MtpWorker).
class TransferManager : public QObject
{
    Q_OBJECT
public:
    enum class State { Running, Done, Failed, Cancelled };

    struct Transfer {
        quint64 id = 0;
        QString title;
        int kind = 0;
        qint64 doneBytes = 0;
        qint64 totalBytes = 0;
        int doneItems = 0;
        int totalItems = 0;
        QString currentName;
        State state = State::Running;
        QString error;
        double bytesPerSecond = 0;
        qint64 lastBytes = 0;
        QElapsedTimer clock;
        qint64 lastMs = 0;

        double fraction() const { return totalBytes > 0 ? double(doneBytes) / double(totalBytes) : 0.0; }
        qint64 secondsLeft() const;
    };

    explicit TransferManager(MtpWorker *worker, QObject *parent = nullptr);

    const QVector<Transfer> &transfers() const { return m_items; }
    int activeCount() const;
    double overallFraction() const;
    void clearFinished();
    void cancel(quint64 id);

signals:
    void changed();
    void finished(quint64 id, bool ok, bool cancelled, const QString &title, const QString &error);

private:
    Transfer *find(quint64 id);
    MtpWorker *m_worker;
    QVector<Transfer> m_items;
};
