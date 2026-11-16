#include "app/AppController.h"
#include "app/Settings.h"
#include "app/Theme.h"
#include "ui/BrowserWindow.h"

#include <QTimer>

#include <QApplication>
#include <QLocale>
#include <QTranslator>

#ifndef NDEBUG
int runSelfTest();
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
#ifndef NDEBUG
    if (QApplication::arguments().contains(QStringLiteral("--selftest")))
        return runSelfTest();
#endif
    QApplication::setApplicationName(QStringLiteral("MacDroid"));
    QApplication::setOrganizationName(QStringLiteral("Bliksund"));
    QApplication::setOrganizationDomain(QStringLiteral("bliksund.com"));
    QApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION_STRING));
    // Lives in the menu bar: closing the browser must not quit.
    QApplication::setQuitOnLastWindowClosed(false);

    const QString lang = Settings::effectiveLanguage();
    QLocale::setDefault(QLocale(lang == QLatin1String("vi") ? QLocale::Vietnamese : QLocale::English));
    QTranslator translator;
    if (translator.load(QStringLiteral(":/i18n/macdroid_%1.qm").arg(lang)))
        QApplication::installTranslator(&translator);

    Theme::instance()->init();

    AppController controller;
#ifndef NDEBUG
    if (!QApplication::arguments().contains(QStringLiteral("--demo")))
#endif
        controller.start();
    // Developer convenience: open the browser window right away.
    const QStringList args = QApplication::arguments();
    if (args.contains(QStringLiteral("--show")))
        controller.showBrowser();
#ifndef NDEBUG
    // --demo [grid] : sample content;  --snapshot <png> : render the window and quit.
    if (args.contains(QStringLiteral("--demo"))) {
        controller.showBrowser();
        controller.browser()->loadDemo(args.contains(QStringLiteral("grid")) ? 1 : 0);
    }
    const int snap = int(args.indexOf(QStringLiteral("--snapshot")));
    if (snap >= 0 && snap + 1 < args.size()) {
        const QString out = args.at(snap + 1);
        QTimer::singleShot(2500, &app, [&controller, out] {
            qWarning("snapshot size %dx%d visible=%d dpr=%f", controller.browser()->width(), controller.browser()->height(), int(controller.browser()->isVisible()), controller.browser()->devicePixelRatioF());
            controller.browser()->grab().save(out);
            QApplication::quit();
        });
    }
#endif
    return QApplication::exec();
}
