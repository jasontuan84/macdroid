#include "core/MtpTypes.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace mtp {

QString Entry::extension() const
{
    const int dot = name.lastIndexOf(u'.');
    if (dot <= 0 || isFolder)
        return {};
    return name.mid(dot + 1).toLower();
}

bool Entry::isImage() const
{
    static const QSet<QString> exts{"jpg", "jpeg", "png", "heic", "heif", "webp", "gif", "bmp", "dng", "avif", "tif", "tiff"};
    return exts.contains(extension());
}

bool Entry::isVideo() const
{
    static const QSet<QString> exts{"mp4", "mov", "m4v", "3gp", "mkv", "webm", "avi"};
    return exts.contains(extension());
}

bool Entry::isPdf() const
{
    return extension() == QLatin1String("pdf");
}

QByteArray encodeItems(const QString &deviceSerial, const ItemRefList &items, bool cut)
{
    QJsonArray arr;
    for (const ItemRef &it : items) {
        arr.append(QJsonObject{
            {"id", double(it.id)},
            {"storage", double(it.storageId)},
            {"parent", double(it.parentId)},
            {"name", it.name},
            {"size", double(it.size)},
            {"folder", it.isFolder},
        });
    }
    return QJsonDocument(QJsonObject{{"device", deviceSerial}, {"items", arr}, {"cut", cut}}).toJson(QJsonDocument::Compact);
}

bool decodeItems(const QByteArray &data, QString *deviceSerial, ItemRefList *items, bool *cut)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    if (deviceSerial)
        *deviceSerial = root.value("device").toString();
    if (cut)
        *cut = root.value("cut").toBool();
    if (items) {
        items->clear();
        for (const QJsonValue &v : root.value("items").toArray()) {
            const QJsonObject o = v.toObject();
            ItemRef r;
            r.id = quint32(o.value("id").toDouble());
            r.storageId = quint32(o.value("storage").toDouble());
            r.parentId = quint32(o.value("parent").toDouble());
            r.name = o.value("name").toString();
            r.size = qint64(o.value("size").toDouble());
            r.isFolder = o.value("folder").toBool();
            items->append(r);
        }
    }
    return true;
}

QString uniqueName(const QString &name, const std::function<bool(const QString &)> &exists)
{
    if (!exists(name))
        return name;
    QString base = name;
    QString ext;
    const int dot = name.lastIndexOf(u'.');
    if (dot > 0) {
        base = name.left(dot);
        ext = name.mid(dot);
    }
    for (int i = 2; i < 10000; ++i) {
        const QString candidate = QStringLiteral("%1 %2%3").arg(base).arg(i).arg(ext);
        if (!exists(candidate))
            return candidate;
    }
    return name;
}

} // namespace mtp
