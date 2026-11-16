#pragma once

#include <QObject>
#include <QString>
#include <QVector>

// Watches USB attach/detach through IOKit without claiming any device.
// Reports which attached devices look like Android phones (by vendor id), so
// the UI can say "phone found" even when MTP is not enabled on it yet.
class UsbMonitor : public QObject
{
    Q_OBJECT
public:
    struct UsbDevice {
        quint64 registryId = 0;
        quint16 vendorId = 0;
        quint16 productId = 0;
        QString productName;
    };

    explicit UsbMonitor(QObject *parent = nullptr);
    ~UsbMonitor() override;

    bool start();
    QVector<UsbDevice> androidDevices() const { return m_android; }

signals:
    void devicesChanged();

private:
    static void onMatched(void *refcon, unsigned int iterator);
    static void onTerminated(void *refcon, unsigned int iterator);
    void drain(unsigned int iterator, bool added);

    void *m_port = nullptr;   // IONotificationPortRef
    unsigned int m_addedIter = 0;
    unsigned int m_removedIter = 0;
    QVector<UsbDevice> m_android;
};
