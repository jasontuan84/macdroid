#pragma once

#include "core/MtpTypes.h"

#include <QAbstractTableModel>
#include <QCollator>
#include <QHash>
#include <QImage>
#include <QSortFilterProxyModel>

// Flat model of one device folder.
class FileModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { NameColumn, DateColumn, SizeColumn, KindColumn, ColumnCount };
    enum Role {
        EntryIdRole = Qt::UserRole + 1,
        IsFolderRole,
        SizeRole,
        DateRole,
        KindRole,
        ThumbnailRole,
        HasThumbnailRole,
        ExtensionRole,
    };

    explicit FileModel(QObject *parent = nullptr);

    void setEntries(quint32 storageId, quint32 parentId, const mtp::EntryList &entries);
    void clear();
    void setThumbnail(quint32 id, const QImage &image);
    void setDropTarget(quint32 id);

    quint32 storageId() const { return m_storageId; }
    quint32 parentId() const { return m_parentId; }
    const mtp::Entry &entry(int row) const { return m_entries.at(row); }
    const mtp::EntryList &entries() const { return m_entries; }
    int rowForId(quint32 id) const;
    int rowForName(const QString &name) const;
    bool containsName(const QString &name) const { return rowForName(name) >= 0; }
    bool hasThumbnail(quint32 id) const { return m_thumbs.contains(id); }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

signals:
    void renameRequested(quint32 id, const QString &newName);

private:
    quint32 m_storageId = 0;
    quint32 m_parentId = mtp::kRootId;
    mtp::EntryList m_entries;
    QHash<quint32, QImage> m_thumbs;
    quint32 m_dropTarget = 0;
};

// Sorting (folders always first, natural name order) and search filtering.
class FileSortProxy : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit FileSortProxy(QObject *parent = nullptr);
    void setShowHidden(bool show);
    void setSearchText(const QString &text);

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QCollator m_collator;
    bool m_showHidden = false;
    QString m_search;
};

QString formatModified(const QDateTime &dt);
