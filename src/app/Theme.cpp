#include "app/Theme.h"

#include "app/Settings.h"
#include "platform/MacPlatform.h"

#include <QActionGroup>
#include <QApplication>
#include <QMenu>
#include <QStyleHints>
#include <QTimer>
#include <QWidget>

namespace {
constexpr const char *kSecondaryProp = "macdroidSecondaryAlpha";
}

Theme *Theme::instance()
{
    static Theme *t = new Theme(qApp);
    return t;
}

void Theme::init()
{
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        // The palette is updated right after the signal: refresh on the next turn.
        QTimer::singleShot(0, this, [this] {
            refreshWidgets();
            emit changed();
        });
    });
    apply(mode());
}

QString Theme::mode() const
{
    return Settings::theme();
}

void Theme::setMode(const QString &mode)
{
    if (mode == Settings::theme())
        return;
    Settings::setTheme(mode);
    apply(mode);
    QTimer::singleShot(0, this, [this] {
        refreshWidgets();
        emit changed();
    });
}

void Theme::apply(const QString &mode)
{
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown; // follow the system
    if (mode == QLatin1String("light"))
        scheme = Qt::ColorScheme::Light;
    else if (mode == QLatin1String("dark"))
        scheme = Qt::ColorScheme::Dark;
    QGuiApplication::styleHints()->setColorScheme(scheme);
    MacPlatform::setAppearance(mode);
}

void Theme::setSecondary(QWidget *w, qreal alpha)
{
    w->setProperty(kSecondaryProp, alpha);
    QPalette pal = QApplication::palette(w);
    QColor c = pal.color(QPalette::WindowText);
    c.setAlphaF(float(alpha));
    pal.setColor(QPalette::WindowText, c);
    pal.setColor(QPalette::Text, c);
    w->setPalette(pal);
}

void Theme::refreshWidgets()
{
    for (QWidget *w : QApplication::allWidgets()) {
        const QVariant a = w->property(kSecondaryProp);
        if (a.isValid())
            setSecondary(w, a.toReal());
        w->update();
    }
}

void Theme::populateMenu(QMenu *menu)
{
    auto *group = new QActionGroup(menu);
    const QList<QPair<QString, QString>> modes{{"system", tr("System")}, {"light", tr("Light")}, {"dark", tr("Dark")}};
    for (const auto &m : modes) {
        QAction *a = menu->addAction(m.second);
        a->setCheckable(true);
        a->setChecked(mode() == m.first);
        a->setData(m.first);
        group->addAction(a);
        const QString code = m.first;
        connect(a, &QAction::triggered, this, [this, code] { setMode(code); });
    }
    connect(this, &Theme::changed, group, [this, group] {
        for (QAction *a : group->actions())
            a->setChecked(a->data().toString() == mode());
    });
}
