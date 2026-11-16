#pragma once

#include <QObject>
#include <QString>

class QWidget;
class QMenu;

// Appearance: "system", "light" or "dark". Applies to Qt widgets and to the
// native AppKit parts (menus, dialogs, title bar) and refreshes widgets that
// derive colours from the palette when the scheme changes at runtime.
class Theme : public QObject
{
    Q_OBJECT
public:
    static Theme *instance();

    void init();                       // apply the saved mode; call once at startup
    QString mode() const;
    void setMode(const QString &mode); // persists and applies immediately

    // Fills `menu` with System / Light / Dark checkable actions kept in sync.
    void populateMenu(QMenu *menu);

    // Marks a widget's text as secondary (WindowText at `alpha`), re-applied
    // automatically after every theme change.
    static void setSecondary(QWidget *w, qreal alpha);

signals:
    void changed();

private:
    using QObject::QObject;
    void apply(const QString &mode);
    void refreshWidgets();
};
