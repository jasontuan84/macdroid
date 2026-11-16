#include "ui/Icons.h"

#include "platform/MacPlatform.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>

namespace Icons {

QPixmap symbol(const QString &name, qreal pointSize, const QColor &color, qreal dpr, int weight)
{
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(name).arg(pointSize).arg(color.rgba()).arg(dpr).arg(weight);
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    QPixmap pm = QPixmap::fromImage(MacPlatform::symbolImage(name, pointSize, color, dpr, weight));
    cache.insert(key, pm);
    return pm;
}

QPixmap filePixmap(const QString &extension, bool isFolder, int logicalSize, qreal dpr)
{
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(extension).arg(isFolder).arg(logicalSize).arg(dpr);
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    QImage img = MacPlatform::fileTypeImage(extension, isFolder, qRound(logicalSize * dpr));
    img.setDevicePixelRatio(dpr);
    QPixmap pm = QPixmap::fromImage(img);
    cache.insert(key, pm);
    return pm;
}

QIcon fileIcon(const QString &extension, bool isFolder)
{
    static QHash<QString, QIcon> cache;
    const QString key = extension + (isFolder ? QStringLiteral("|d") : QStringLiteral("|f"));
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    QIcon icon;
    for (int s : {16, 32, 64, 128, 256}) {
        QImage img = MacPlatform::fileTypeImage(extension, isFolder, s);
        icon.addPixmap(QPixmap::fromImage(img));
    }
    cache.insert(key, icon);
    return icon;
}

QString kind(const QString &extension, bool isFolder)
{
    static QHash<QString, QString> cache;
    const QString key = extension + (isFolder ? QStringLiteral("|d") : QStringLiteral("|f"));
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return *it;
    const QString k = MacPlatform::kindForExtension(extension, isFolder);
    cache.insert(key, k);
    return k;
}

QIcon trayIcon(TrayState state, double progress)
{
    // 18x18 pt template image drawn at 2x. Only alpha matters (template).
    const qreal dpr = 2.0;
    QPixmap pm(QSize(18, 18) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor ink(0, 0, 0);
    const QRectF phone(5, 1.5, 8.5, 15);

    switch (state) {
    case TrayState::NoDevice: {
        QColor c = ink;
        c.setAlphaF(0.55);
        p.setPen(QPen(c, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(phone, 2, 2);
        p.drawLine(QPointF(8.2, 14), QPointF(10.3, 14));
        break;
    }
    case TrayState::Attention: {
        p.setPen(QPen(ink, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(phone, 2, 2);
        p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(9.25, 5), QPointF(9.25, 9.5));
        p.drawPoint(QPointF(9.25, 12.2));
        break;
    }
    case TrayState::Connected:
    case TrayState::Busy: {
        // Connected: a colour (non-template) green phone so it stands out.
        const QColor green(48, 209, 88);
        QPainterPath body;
        body.addRoundedRect(phone, 2.2, 2.2);
        p.fillPath(body, green);
        if (state == TrayState::Busy) {
            // Progress: the body empties from the top as the transfer advances.
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            const qreal h = (phone.height() - 3) * (1.0 - qBound(0.0, progress, 1.0));
            p.fillRect(QRectF(phone.left() + 1.5, phone.top() + 1.5, phone.width() - 3, h), Qt::transparent);
        } else {
            p.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath check;
            check.moveTo(6.9, 9.2);
            check.lineTo(8.7, 11);
            check.lineTo(11.7, 7.3);
            p.drawPath(check);
        }
        break;
    }
    }
    p.end();
    QIcon icon(pm);
    // Template (adapts to the menu bar) except when connected, which is green.
    icon.setIsMask(state == TrayState::NoDevice || state == TrayState::Attention);
    return icon;
}

} // namespace Icons
