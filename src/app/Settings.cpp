#include "app/Settings.h"

#include <QLocale>
#include <QSettings>

namespace Settings {

namespace {
QSettings &store()
{
    static QSettings s;
    return s;
}
}

bool openBrowserOnConnect() { return store().value("openBrowserOnConnect", true).toBool(); }
void setOpenBrowserOnConnect(bool on) { store().setValue("openBrowserOnConnect", on); }

bool showHiddenFiles() { return store().value("showHiddenFiles", false).toBool(); }
void setShowHiddenFiles(bool on) { store().setValue("showHiddenFiles", on); }

QString language() { return store().value("language", "system").toString(); }
void setLanguage(const QString &code) { store().setValue("language", code); }

QString effectiveLanguage()
{
    const QString lang = language();
    if (lang == QLatin1String("en") || lang == QLatin1String("vi"))
        return lang;
    for (const QString &ui : QLocale::system().uiLanguages()) {
        if (ui.startsWith(QLatin1String("vi")))
            return QStringLiteral("vi");
        if (ui.startsWith(QLatin1String("en")))
            return QStringLiteral("en");
    }
    return QStringLiteral("en");
}

QString theme() { return store().value("theme", "system").toString(); }
void setTheme(const QString &mode) { store().setValue("theme", mode); }

int viewMode() { return store().value("viewMode", 0).toInt(); }
void setViewMode(int mode) { store().setValue("viewMode", mode); }

int sortColumn() { return store().value("sortColumn", 0).toInt(); }
void setSortColumn(int column) { store().setValue("sortColumn", column); }

bool sortDescending() { return store().value("sortDescending", false).toBool(); }
void setSortDescending(bool descending) { store().setValue("sortDescending", descending); }

} // namespace Settings
