#pragma once

#include <QString>

// Typed access to persisted user preferences (QSettings under the hood).
namespace Settings {

bool openBrowserOnConnect();
void setOpenBrowserOnConnect(bool on);

bool showHiddenFiles();
void setShowHiddenFiles(bool on);

// "system", "en" or "vi"
QString language();
void setLanguage(const QString &code);
// Resolves "system" to the effective UI language ("en" or "vi").
QString effectiveLanguage();

// "system", "light" or "dark"
QString theme();
void setTheme(const QString &mode);

int viewMode();          // 0 = list, 1 = grid
void setViewMode(int mode);

int sortColumn();        // FileModel::Column
void setSortColumn(int column);
bool sortDescending();
void setSortDescending(bool descending);

} // namespace Settings
