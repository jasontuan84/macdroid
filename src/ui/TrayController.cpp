#include "ui/TrayController.h"

#include "app/Settings.h"
#include "app/Theme.h"
#include "core/TransferManager.h"
#include "platform/MacPlatform.h"
#include "ui/TransfersPopup.h"

#include <QAction>
#include <QActionGroup>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QSystemTrayIcon>

TrayController::TrayController(TransferManager *transfers, QObject *parent)
    : QObject(parent)
    , m_transfers(transfers)
    , m_tray(new QSystemTrayIcon(this))
    , m_menu(new QMenu())
{
    m_tray->setContextMenu(m_menu);
    m_tray->setToolTip(QStringLiteral("MacDroid"));
    connect(m_transfers, &TransferManager::changed, this, [this] {
        refreshIcon();
        if (!m_menu->isVisible())
            rebuild();
    });
    rebuild();
    refreshIcon();
    m_tray->show();
}

TrayController::~TrayController()
{
    delete m_menu;
}

void TrayController::setNoDevice()
{
    m_mode = Mode::NoDevice;
    m_device = {};
    rebuild();
    refreshIcon();
}

void TrayController::setAttention(const QString &deviceName, const QString &message, bool heldByImageCapture)
{
    m_heldByImageCapture = heldByImageCapture;
    m_mode = Mode::Attention;
    m_deviceName = deviceName;
    m_message = message;
    rebuild();
    refreshIcon();
}

void TrayController::setConnected(const mtp::DeviceInfo &info)
{
    m_mode = Mode::Connected;
    m_device = info;
    rebuild();
    refreshIcon();
}

void TrayController::updateStorages(const mtp::StorageList &storages)
{
    m_device.storages = storages;
    if (!m_menu->isVisible())
        rebuild();
}

void TrayController::refreshIcon()
{
    Icons::TrayState state = Icons::TrayState::NoDevice;
    QString tip = tr("MacDroid: no device");
    if (m_mode == Mode::Attention) {
        state = Icons::TrayState::Attention;
        tip = tr("MacDroid: action needed on the phone");
    } else if (m_mode == Mode::Connected) {
        state = m_transfers->activeCount() > 0 ? Icons::TrayState::Busy : Icons::TrayState::Connected;
        tip = tr("MacDroid: %1 connected").arg(m_device.name);
    }
    m_tray->setIcon(Icons::trayIcon(state, m_transfers->overallFraction()));
    m_tray->setToolTip(tip);
}

void TrayController::addSettings(QMenu *menu)
{
    QMenu *settings = menu->addMenu(tr("Settings"));
    QAction *autoOpen = settings->addAction(tr("Open Browser When a Phone Connects"));
    autoOpen->setCheckable(true);
    autoOpen->setChecked(Settings::openBrowserOnConnect());
    connect(autoOpen, &QAction::toggled, this, [](bool on) { Settings::setOpenBrowserOnConnect(on); });

    QAction *login = settings->addAction(tr("Launch at Login"));
    login->setCheckable(true);
    login->setChecked(MacPlatform::launchAtLoginEnabled());
    connect(login, &QAction::toggled, this, [](bool on) {
        QString error;
        if (!MacPlatform::setLaunchAtLogin(on, &error) && !error.isEmpty())
            QMessageBox::warning(nullptr, tr("Launch at Login"), error);
    });

    Theme::instance()->populateMenu(settings->addMenu(tr("Appearance")));

    QMenu *lang = settings->addMenu(tr("Language"));
    auto *group = new QActionGroup(lang);
    const QList<QPair<QString, QString>> langs{{"system", tr("System Default")}, {"en", QStringLiteral("English")},
                                               {"vi", QStringLiteral("Tiếng Việt")}};
    for (const auto &l : langs) {
        QAction *a = lang->addAction(l.second);
        a->setCheckable(true);
        a->setChecked(Settings::language() == l.first);
        group->addAction(a);
        const QString code = l.first;
        connect(a, &QAction::triggered, this, [code] {
            if (Settings::language() == code)
                return;
            Settings::setLanguage(code);
            QMessageBox::information(nullptr, tr("Language"),
                                     tr("The new language will be used the next time MacDroid starts."));
        });
    }
}

void TrayController::rebuild()
{
    m_menu->clear();
    const QLocale loc;

    switch (m_mode) {
    case Mode::NoDevice: {
        QAction *h = m_menu->addAction(tr("No Android Device"));
        h->setEnabled(false);
        QAction *hint = m_menu->addAction(tr("Connect a phone with a USB cable"));
        hint->setEnabled(false);
        m_menu->addSeparator();
        connect(m_menu->addAction(tr("Open MacDroid")), &QAction::triggered, this, &TrayController::openBrowserRequested);
        break;
    }
    case Mode::Attention: {
        QAction *h = m_menu->addAction(m_deviceName.isEmpty() ? tr("Android Phone") : m_deviceName);
        h->setEnabled(false);
        QAction *msg = m_menu->addAction(m_heldByImageCapture ? tr("In use by Image Capture") : tr("Waiting for permission"));
        msg->setEnabled(false);
        if (!m_heldByImageCapture) {
            m_menu->addSeparator();
            for (const QString &step : {tr("1. Unlock your phone"), tr("2. Tap the \"USB\" notification"), tr("3. Choose \"File transfer\"")}) {
                QAction *s = m_menu->addAction(step);
                s->setEnabled(false);
            }
        }
        m_menu->addSeparator();
        if (m_heldByImageCapture)
            connect(m_menu->addAction(tr("Release Phone from Image Capture")), &QAction::triggered, this, &TrayController::releaseRequested);
        else
            connect(m_menu->addAction(tr("Try Again")), &QAction::triggered, this, &TrayController::retryRequested);
        connect(m_menu->addAction(tr("Open MacDroid")), &QAction::triggered, this, &TrayController::openBrowserRequested);
        break;
    }
    case Mode::Connected: {
        QAction *h = m_menu->addAction(m_device.name);
        h->setEnabled(false);
        QString sub = tr("Connected via USB");
        if (m_device.batteryPercent >= 0)
            sub += QStringLiteral(" · ") + tr("Battery %1%").arg(m_device.batteryPercent);
        QAction *s = m_menu->addAction(sub);
        s->setEnabled(false);
        for (const mtp::Storage &st : m_device.storages) {
            QAction *a = m_menu->addAction(tr("%1: %2 free of %3").arg(st.description,
                loc.formattedDataSize(st.freeBytes, 1, QLocale::DataSizeSIFormat),
                loc.formattedDataSize(st.capacity, 0, QLocale::DataSizeSIFormat)));
            a->setEnabled(false);
        }
        m_menu->addSeparator();
        QAction *open = m_menu->addAction(tr("Open MacDroid"));
        open->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
        connect(open, &QAction::triggered, this, &TrayController::openBrowserRequested);

        QMenu *quick = m_menu->addMenu(tr("Quick Open"));
        const QList<QStringList> paths{{"DCIM", "Camera"}, {"Download"}, {"Pictures", "Screenshots"}, {"Documents"}};
        for (const QStringList &p : paths) {
            QAction *a = quick->addAction(p.join(QStringLiteral(" / ")));
            connect(a, &QAction::triggered, this, [this, p] { emit openPathRequested(p); });
        }

        // Active transfers.
        bool header = false;
        for (const TransferManager::Transfer &t : m_transfers->transfers()) {
            if (t.state != TransferManager::State::Running)
                continue;
            if (!header) {
                m_menu->addSeparator();
                header = true;
            }
            QAction *a = m_menu->addAction(QStringLiteral("%1  %2%").arg(t.title).arg(int(t.fraction() * 100)));
            a->setEnabled(false);
            QAction *d = m_menu->addAction(describeTransfer(t.doneBytes, t.totalBytes, t.bytesPerSecond, t.secondsLeft()));
            d->setEnabled(false);
        }
        m_menu->addSeparator();
        connect(m_menu->addAction(tr("Eject \"%1\"").arg(m_device.name)), &QAction::triggered, this, &TrayController::ejectRequested);
        break;
    }
    }

    m_menu->addSeparator();
    addSettings(m_menu);
    QAction *quit = m_menu->addAction(tr("Quit MacDroid"));
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::NoRole);
    connect(quit, &QAction::triggered, this, &TrayController::quitRequested);
}
