#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVector>
#include <cstdint>
#include <functional>

namespace mtp {

// MTP uses 0xFFFFFFFF as "root of the storage" when listing children.
constexpr quint32 kRootId = 0xFFFFFFFFu;

struct Entry {
    quint32 id = 0;
    quint32 parentId = 0;
    quint32 storageId = 0;
    QString name;
    qint64 size = 0;
    QDateTime modified;
    bool isFolder = false;

    QString extension() const;   // lower-case, without dot
    bool isImage() const;
    bool isVideo() const;
    bool isPdf() const;
    bool hasThumbnail() const { return isImage() || isVideo() || isPdf(); }
};
using EntryList = QVector<Entry>;

struct Storage {
    quint32 id = 0;
    QString description;   // e.g. "Internal shared storage"
    qint64 capacity = 0;
    qint64 freeBytes = 0;
    bool removable = false;
};
using StorageList = QVector<Storage>;

struct DeviceInfo {
    QString name;          // friendly name, falls back to model
    QString manufacturer;
    QString model;
    QString serial;
    int batteryPercent = -1;
    StorageList storages;
    bool isValid() const { return !serial.isEmpty() || !model.isEmpty(); }
};

// One step of a path on the device (folder id + display name).
struct PathNode {
    quint32 id = kRootId;
    QString name;
};
using Path = QVector<PathNode>;

// Reference to an object on the device, used for clipboard and drag & drop.
struct ItemRef {
    quint32 id = 0;
    quint32 storageId = 0;
    quint32 parentId = 0;
    QString name;
    qint64 size = 0;
    bool isFolder = false;
};
using ItemRefList = QVector<ItemRef>;

QByteArray encodeItems(const QString &deviceSerial, const ItemRefList &items, bool cut = false);
bool decodeItems(const QByteArray &data, QString *deviceSerial, ItemRefList *items, bool *cut = nullptr);

enum class ConflictPolicy { Ask, Replace, KeepBoth, Skip, Stop };

// "photo.jpg" -> "photo 2.jpg", "photo 3.jpg"... like Finder.
QString uniqueName(const QString &name, const std::function<bool(const QString &)> &exists);

} // namespace mtp

Q_DECLARE_METATYPE(mtp::Entry)
Q_DECLARE_METATYPE(mtp::EntryList)
Q_DECLARE_METATYPE(mtp::DeviceInfo)
Q_DECLARE_METATYPE(mtp::StorageList)
Q_DECLARE_METATYPE(mtp::Path)
