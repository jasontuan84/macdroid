#pragma once

#include "core/MtpTypes.h"

#include <QObject>
#include <QTimer>

class MtpWorker;
class UsbMonitor;
class TransferManager;
class TrayController;
class BrowserWindow;

// Wires USB detection, the MTP session, the menu bar extra and the browser
// window together, and owns the connection state machine:
//   NoDevice -> Attention (phone seen, MTP not usable yet) -> Connected
class AppController : public QObject
{
    Q_OBJECT
public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    void start();
    void showBrowser();
    BrowserWindow *browser() const { return m_browser; }

private:
    enum class State { NoDevice, Attention, Connected };

    void scheduleRescan(int delayMs = 700);
    void enterNoDevice();
    void enterAttention(const QString &message, bool heldByImageCapture = false);
    void retry();
    void enterConnected(const mtp::DeviceInfo &info);
    void eject();
    void quit();
    QString usbPhoneName() const;

    MtpWorker *m_worker;
    UsbMonitor *m_usb;
    TransferManager *m_transfers;
    TrayController *m_tray;
    BrowserWindow *m_browser;
    QTimer m_rescanTimer;
    QTimer m_pollTimer;
    State m_state = State::NoDevice;
    bool m_ejected = false;
    QString m_lastSerial;
};
