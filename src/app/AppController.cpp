#include "app/AppController.h"

#include "app/Settings.h"
#include "core/MtpWorker.h"
#include "core/TransferManager.h"
#include "core/UsbMonitor.h"
#include "platform/MacPlatform.h"
#include "ui/BrowserWindow.h"
#include "ui/TrayController.h"

#include <QApplication>
#include <QMessageBox>

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_worker(new MtpWorker(this))
    , m_usb(new UsbMonitor(this))
    , m_transfers(new TransferManager(m_worker, this))
    , m_tray(new TrayController(m_transfers, this))
    , m_browser(new BrowserWindow(m_worker, m_transfers))
{
    m_rescanTimer.setSingleShot(true);
    connect(&m_rescanTimer, &QTimer::timeout, m_worker, [this] { m_worker->rescan(); });
    // While waiting for the user (locked phone, charging mode) poll gently.
    m_pollTimer.setInterval(2500);
    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        if (!m_ejected)
            m_worker->rescan();
    });

    connect(m_usb, &UsbMonitor::devicesChanged, this, [this] {
        if (m_usb->androidDevices().isEmpty())
            m_ejected = false; // unplugged: a new plug-in connects again
        scheduleRescan();
    });

    connect(m_worker, &MtpWorker::deviceOpened, this, [this](const mtp::DeviceInfo &info) {
        if (m_ejected)
            return;
        enterConnected(info);
    });
    connect(m_worker, &MtpWorker::deviceClosed, this, [this] {
        if (!m_usb->androidDevices().isEmpty() && !m_ejected)
            enterAttention(tr("Turn on \"File transfer\" on the phone to browse its files."));
        else
            enterNoDevice();
        scheduleRescan(1200);
    });
    connect(m_worker, &MtpWorker::noDevice, this, [this] {
        if (!m_usb->androidDevices().isEmpty() && !m_ejected)
            enterAttention(tr("The phone is connected for charging only. Turn on \"File transfer\" on the phone."));
        else
            enterNoDevice();
    });
    connect(m_worker, &MtpWorker::deviceNotReady, this, [this](const QString &reason, bool) {
        if (!m_ejected)
            enterAttention(reason);
    });
    connect(m_worker, &MtpWorker::deviceHeldByImageCapture, this, [this] {
        if (!m_ejected)
            enterAttention(tr("macOS Image Capture is using the phone, so MacDroid can't access it. "
                              "Click \"Release Phone\" to let MacDroid take over the connection."), true);
    });
    connect(m_worker, &MtpWorker::storagesChanged, this, [this](const mtp::StorageList &s) {
        m_tray->updateStorages(s);
        m_browser->updateStorages(s);
    });

    connect(m_tray, &TrayController::openBrowserRequested, this, &AppController::showBrowser);
    connect(m_tray, &TrayController::openPathRequested, this, [this](const QStringList &parts) {
        showBrowser();
        m_browser->openPath(parts);
    });
    connect(m_tray, &TrayController::retryRequested, this, &AppController::retry);
    connect(m_tray, &TrayController::releaseRequested, this, [this] {
        m_ejected = false;
        m_worker->releaseFromImageCapture();
    });
    connect(m_tray, &TrayController::ejectRequested, this, &AppController::eject);
    connect(m_tray, &TrayController::quitRequested, this, &AppController::quit);

    connect(m_browser, &BrowserWindow::windowHidden, this, [] { MacPlatform::setDockIconVisible(false); });
    connect(m_browser, &BrowserWindow::retryRequested, this, &AppController::retry);
    connect(m_browser, &BrowserWindow::releaseRequested, this, [this] {
        m_ejected = false;
        m_worker->releaseFromImageCapture();
    });
    connect(m_browser, &BrowserWindow::ejectRequested, this, &AppController::eject);

    connect(m_transfers, &TransferManager::finished, this,
            [this](quint64, bool ok, bool cancelled, const QString &title, const QString &) {
        // Tell the user about long jobs finishing while the window is hidden.
        if (ok && !cancelled && !m_browser->isActiveWindow())
            MacPlatform::postNotification(tr("Transfer complete"), title);
    });
}

AppController::~AppController()
{
    m_worker->stop();
    delete m_browser;
}

void AppController::start()
{
    MacPlatform::initNotifications([this] { showBrowser(); });
    m_worker->start();
    m_usb->start();
    m_worker->rescan();
}

void AppController::showBrowser()
{
    MacPlatform::setDockIconVisible(true);
    m_browser->show();
    m_browser->raise();
    m_browser->activateWindow();
    MacPlatform::activateApp();
}

void AppController::scheduleRescan(int delayMs)
{
    // Phones re-enumerate several times when switching USB modes: debounce.
    m_rescanTimer.start(delayMs);
}

QString AppController::usbPhoneName() const
{
    const auto devices = m_usb->androidDevices();
    return devices.isEmpty() ? QString() : devices.first().productName;
}

void AppController::enterNoDevice()
{
    m_pollTimer.stop();
    if (m_state == State::NoDevice)
        return;
    m_state = State::NoDevice;
    m_tray->setNoDevice();
    m_browser->setNoDevice();
}

void AppController::enterAttention(const QString &message, bool heldByImageCapture)
{
    // Polling would only bounce off Image Capture again: wait for the user.
    if (heldByImageCapture)
        m_pollTimer.stop();
    else
        m_pollTimer.start();
    m_state = State::Attention;
    m_tray->setAttention(usbPhoneName(), message, heldByImageCapture);
    m_browser->setAttention(message, heldByImageCapture);
}

void AppController::retry()
{
    m_ejected = false;
    m_worker->rescan();
}

void AppController::enterConnected(const mtp::DeviceInfo &info)
{
    m_pollTimer.stop();
    const bool fresh = m_state != State::Connected || info.serial != m_lastSerial;
    m_state = State::Connected;
    m_lastSerial = info.serial;
    m_tray->setConnected(info);
    m_browser->setDevice(info);
    if (fresh) {
        MacPlatform::postNotification(tr("%1 connected").arg(info.name), tr("Click to browse the phone's files."));
        if (Settings::openBrowserOnConnect())
            showBrowser();
    }
}

void AppController::eject()
{
    if (m_transfers->activeCount() > 0) {
        const auto answer = QMessageBox::question(nullptr, tr("Eject"),
            tr("Transfers are still running. Eject the phone anyway?"));
        if (answer != QMessageBox::Yes)
            return;
    }
    m_ejected = true;
    m_worker->stop();
    m_worker->start();
    enterNoDevice();
    m_browser->setNoDevice();
}

void AppController::quit()
{
    if (m_transfers->activeCount() > 0) {
        const auto answer = QMessageBox::question(nullptr, tr("Quit MacDroid"),
            tr("Transfers are still running. Quit anyway?"));
        if (answer != QMessageBox::Yes)
            return;
    }
    qApp->quit();
}
