#include "ui/FileModel.h"

#include "ui/Icons.h"

#include <QFont>
#include <QLocale>

QString formatModified(const QDateTime &dt)
{
    if (!dt.isValid() || dt.toSecsSinceEpoch() <= 0)
        return QStringLiteral("--");
    const QLocale loc;
    const QDate today = QDate::currentDate();
    const QString time = loc.toString(dt.time(), QLocale::ShortFormat);
    if (dt.date() == today)
        return FileModel::tr("Today, %1").arg(time);
    if (dt.date() == today.addDays(-1))
        return FileModel::tr("Yesterday, %1").arg(time);
    // Finder style: "12 Nov 2026, 18:42" (localised month name).
    return QStringLiteral("%1, %2").arg(loc.toString(dt.date(), QStringLiteral("d MMM yyyy")), time);
}

FileModel::FileModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void FileModel::setEntries(quint32 storageId, quint32 parentId, const mtp::EntryList &entries)
{
    beginResetModel();
    if (storageId != m_storageId || parentId != m_parentId)
        m_thumbs.clear();
    m_storageId = storageId;
    m_parentId = parentId;
    m_entries = entries;
    m_dropTarget = 0;
    endResetModel();
}

void FileModel::clear()
{
    beginResetModel();
    m_entries.clear();
    m_thumbs.clear();
    endResetModel();
}

void FileModel::setThumbnail(quint32 id, const QImage &image)
{
    const int row = rowForId(id);
    if (row < 0)
        return;
    m_thumbs.insert(id, image);
    const QModelIndex idx = index(row, NameColumn);
    emit dataChanged(idx, idx, {ThumbnailRole});
}

void FileModel::setDropTarget(quint32 id)
{
    if (id == m_dropTarget)
        return;
    const int oldRow = rowForId(m_dropTarget);
    m_dropTarget = id;
    const int newRow = rowForId(id);
    for (int row : {oldRow, newRow}) {
        if (row >= 0)
            emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
    }
}

int FileModel::rowForId(quint32 id) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).id == id)
            return i;
    return -1;
}

int FileModel::rowForName(const QString &name) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

int FileModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

int FileModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant FileModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const mtp::Entry &e = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case NameColumn: return e.name;
        case DateColumn: return formatModified(e.modified);
        case SizeColumn: return e.isFolder ? QStringLiteral("--") : QLocale().formattedDataSize(e.size, 1, QLocale::DataSizeSIFormat);
        case KindColumn: return Icons::kind(e.extension(), e.isFolder);
        }
        break;
    case Qt::EditRole:
        return e.name;
    case Qt::DecorationRole:
        if (index.column() == NameColumn)
            return Icons::fileIcon(e.extension(), e.isFolder);
        break;
    case Qt::TextAlignmentRole:
        if (index.column() == SizeColumn)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ForegroundRole:
        if (index.column() != NameColumn)
            return QVariant(); // the view dims secondary columns via palette
        break;
    case Qt::BackgroundRole:
        if (m_dropTarget && e.id == m_dropTarget)
            return QColor(10, 122, 255, 60);
        break;
    case Qt::ToolTipRole:
        return e.name;
    case EntryIdRole: return e.id;
    case IsFolderRole: return e.isFolder;
    case SizeRole: return e.size;
    case DateRole: return e.modified;
    case KindRole: return Icons::kind(e.extension(), e.isFolder);
    case ExtensionRole: return e.extension();
    case HasThumbnailRole: return e.hasThumbnail();
    case ThumbnailRole: {
        auto it = m_thumbs.constFind(e.id);
        return it == m_thumbs.constEnd() ? QVariant() : QVariant(*it);
    }
    }
    return {};
}

QVariant FileModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case NameColumn: return tr("Name");
        case DateColumn: return tr("Date Modified");
        case SizeColumn: return tr("Size");
        case KindColumn: return tr("Kind");
        }
    }
    if (role == Qt::TextAlignmentRole)
        return int((section == SizeColumn ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
    return {};
}

Qt::ItemFlags FileModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (index.column() == NameColumn)
        f |= Qt::ItemIsEditable;
    if (m_entries.at(index.row()).isFolder)
        f |= Qt::ItemIsDropEnabled;
    return f;
}

bool FileModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || index.column() != NameColumn)
        return false;
    const QString name = value.toString().trimmed();
    mtp::Entry &e = m_entries[index.row()];
    if (name.isEmpty() || name == e.name || name.contains(u'/'))
        return false;
    emit renameRequested(e.id, name);
    // Optimistic update; the browser reloads the folder after the rename.
    e.name = name;
    emit dataChanged(index, this->index(index.row(), ColumnCount - 1));
    return true;
}

// ------------------------------------------------------------------ proxy

FileSortProxy::FileSortProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    setDynamicSortFilter(true);
}

void FileSortProxy::setShowHidden(bool show)
{
    beginFilterChange();
    m_showHidden = show;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void FileSortProxy::setSearchText(const QString &text)
{
    beginFilterChange();
    m_search = text.trimmed();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool FileSortProxy::filterAcceptsRow(int sourceRow, const QModelIndex &) const
{
    auto *m = static_cast<FileModel *>(sourceModel());
    const mtp::Entry &e = m->entry(sourceRow);
    if (!m_showHidden && e.name.startsWith(u'.'))
        return false;
    if (!m_search.isEmpty() && !e.name.contains(m_search, Qt::CaseInsensitive))
        return false;
    return true;
}

bool FileSortProxy::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    auto *m = static_cast<FileModel *>(sourceModel());
    const mtp::Entry &a = m->entry(left.row());
    const mtp::Entry &b = m->entry(right.row());
    const bool ascending = sortOrder() == Qt::AscendingOrder;

    // Folders first regardless of direction: compensate for the proxy's reversal.
    if (a.isFolder != b.isFolder)
        return ascending ? a.isFolder : b.isFolder;

    int cmp = 0;
    switch (left.column()) {
    case FileModel::DateColumn:
        cmp = a.modified < b.modified ? -1 : (a.modified > b.modified ? 1 : 0);
        break;
    case FileModel::SizeColumn:
        cmp = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        break;
    case FileModel::KindColumn:
        cmp = m_collator.compare(Icons::kind(a.extension(), a.isFolder), Icons::kind(b.extension(), b.isFolder));
        break;
    default:
        break;
    }
    if (cmp == 0)
        cmp = m_collator.compare(a.name, b.name);
    return cmp < 0;
}
