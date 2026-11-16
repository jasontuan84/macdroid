#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace Icons {

// SF Symbol as a pixmap in the given colour (cached).
QPixmap symbol(const QString &name, qreal pointSize, const QColor &color, qreal dpr, int weight = 0);
// Native Finder icon for a file extension / folder (cached).
QIcon fileIcon(const QString &extension, bool isFolder);
QPixmap filePixmap(const QString &extension, bool isFolder, int logicalSize, qreal dpr);
// Localised "Kind" text from the system (cached).
QString kind(const QString &extension, bool isFolder);

enum class TrayState { NoDevice, Attention, Connected, Busy };
// Monochrome template icon for the menu bar.
QIcon trayIcon(TrayState state, double progress = 0.0);

} // namespace Icons
