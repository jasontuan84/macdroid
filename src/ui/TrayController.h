#pragma once

#include "core/MtpTypes.h"
#include "ui/Icons.h"

#include <QObject>
#include <QStringList>

class QMenu;
class QSystemTrayIcon;
class TransferManager;

// Menu bar extra: status icon plus a menu that reflects the connection state.
class TrayController : public QObject
{
    Q_OBJECT
public:
    explicit TrayController(TransferManager *transfers, QObject *parent = nullptr);
    ~TrayController() override;

    void setNoDevice();
    void setAttention(const QString &deviceName, const QString &message, bool heldByImageCapture = false);
    void setConnected(const mtp::DeviceInfo &info);
    void updateStorages(const mtp::StorageList &storages);

signals:
    void openBrowserRequested();
    void openPathRequested(const QStringList &parts);
    void retryRequested();
    void releaseRequested();
    void ejectRequested();
    void quitRequested();

private:
    enum class Mode { NoDevice, Attention, Connected };
    void rebuild();
    void refreshIcon();
    void addSettings(QMenu *menu);

    TransferManager *m_transfers;
    QSystemTrayIcon *m_tray;
    QMenu *m_menu;
    Mode m_mode = Mode::NoDevice;
    mtp::DeviceInfo m_device;
    QString m_deviceName;
    QString m_message;
    bool m_heldByImageCapture = false;
};
