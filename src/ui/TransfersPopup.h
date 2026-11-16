#pragma once

#include <QWidget>

class TransferManager;
class QVBoxLayout;
class QLabel;

// Glass popover listing running and finished transfers.
class TransfersPopup : public QWidget
{
    Q_OBJECT
public:
    explicit TransfersPopup(TransferManager *transfers, QWidget *parent = nullptr);
    void showBelow(QWidget *anchor);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void rebuild();
    TransferManager *m_transfers;
    QVBoxLayout *m_rows;
    QLabel *m_empty;
};

QString describeTransfer(qint64 done, qint64 total, double bytesPerSecond, qint64 secondsLeft);
