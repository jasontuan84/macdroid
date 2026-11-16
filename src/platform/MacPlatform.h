#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QWidget;

// Thin bridge to AppKit / system frameworks for the pieces Qt does not cover:
// Dock visibility, window chrome, native file icons, SF Symbols, QuickLook
// thumbnails, notifications, login item, file-promise drags and a lazy
// pasteboard that lets Finder paste files that are still on the phone.
namespace MacPlatform {

// ---- application / window
void setDockIconVisible(bool visible);
void activateApp();
void styleBrowserWindow(QWidget *window);
// Starts a native window move from the current single-click mouse-down or
// drag event (title bar behaviour). Returns false if AppKit could not start it.
bool startWindowDrag(QWidget *widget);
// Title bar double-click: performs the user's system preference (zoom/minimise).
void titleBarDoubleClick(QWidget *widget);
bool isDarkAppearance();
// "system" (follow macOS), "light" or "dark" for native AppKit UI.
void setAppearance(const QString &mode);

// ---- icons and type info
QImage fileTypeImage(const QString &extension, bool isFolder, int pixelSize);
QString kindForExtension(const QString &extension, bool isFolder);
// SF Symbol rendered in a solid colour. pointSize in points; dpr = device pixel ratio.
QImage symbolImage(const QString &name, qreal pointSize, const QColor &color, qreal dpr, int weight = 0);

// Blocking; safe to call from a worker thread.
QImage quickLookThumbnail(const QString &localPath, int pixelSize);

// ---- notifications
void initNotifications(std::function<void()> onActivate);
void postNotification(const QString &title, const QString &body);

// ---- login item
bool launchAtLoginEnabled();
bool setLaunchAtLogin(bool enabled, QString *error = nullptr);

// ---- drag & drop / clipboard of items that live on the phone
struct PromisedItem {
    QString name;
    bool isFolder = false;
};
inline constexpr const char *kInternalPasteboardType = "com.bliksund.macdroid.items";

// Writer runs on a background thread; it must create `destinationPath`
// (file or folder) and return true on success. Blocking is fine there.
using PromiseWriter = std::function<bool(int index, const QString &destinationPath)>;

// Starts a native drag from `source` (any widget inside a top-level window)
// using NSFilePromiseProvider so Finder receives real files on drop.
// Returns false if no suitable mouse event is available.
bool startFilePromiseDrag(QWidget *source, const QVector<PromisedItem> &items,
                          const QByteArray &internalData, const QImage &preview,
                          PromiseWriter writer, std::function<void()> finished);
bool internalDragActive();
QByteArray internalDragData();
void registerDropTypes(QWidget *topLevel);

// Copy: puts our item list plus lazily-provided file URLs on the general
// pasteboard. `provideFile` is called on the main thread when another app
// (Finder) actually pastes; it returns a local path (or empty on failure).
void writeItemsToPasteboard(const QVector<PromisedItem> &items, const QByteArray &internalData,
                            std::function<QString(int index)> provideFile);
bool pasteboardHasInternalItems();
void clearPasteboard();
QByteArray pasteboardInternalItems();
// Local file paths on the pasteboard (e.g. copied in Finder). Never triggers
// our own lazy provider.
QStringList pasteboardFilePaths();

} // namespace MacPlatform
