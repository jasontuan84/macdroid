#pragma once

#include <QListView>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeView>

class FileModel;

// Drag & drop and drag-start behaviour shared by the list and grid views.
// Drops are not routed through the model: the view reports what was dropped
// and where, and the browser decides (upload / move / copy).
class FileViewDnd : public QObject
{
    Q_OBJECT
public:
    // Double-click on an item: emitted with the view (proxy) index.
    void requestOpen(const QModelIndex &index) { emit openRequested(index); }

    FileViewDnd(QAbstractItemView *view, FileModel *model);

    bool dragEnter(QDragEnterEvent *e);
    bool dragMove(QDragMoveEvent *e);
    void dragLeave();
    bool drop(QDropEvent *e);
    bool dropActive() const { return m_active && m_targetId == 0; }

signals:
    // targetFolderId == 0 means "the folder being shown".
    void filesDropped(const QStringList &localPaths, quint32 targetFolderId);
    void itemsDropped(const QByteArray &internalData, quint32 targetFolderId, bool copy);
    void startDragRequested();
    void openRequested(const QModelIndex &index);

private:
    quint32 folderAt(const QPoint &viewportPos) const;
    bool acceptable(const QMimeData *md) const;
    void setTarget(quint32 id, bool active);

    QAbstractItemView *m_view;
    FileModel *m_model;
    quint32 m_targetId = 0;
    bool m_active = false;
};

class FileListView : public QTreeView
{
    Q_OBJECT
public:
    FileListView(FileModel *model, QWidget *parent = nullptr);
    FileViewDnd *dnd() const { return m_dnd; }
protected:
    void startDrag(Qt::DropActions) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
private:
    FileViewDnd *m_dnd;
};

class FileGridView : public QListView
{
    Q_OBJECT
public:
    FileGridView(FileModel *model, QWidget *parent = nullptr);
    FileViewDnd *dnd() const { return m_dnd; }
    // Source-model rows currently visible (for lazy thumbnails).
    QList<int> visibleSourceRows() const;
signals:
    void visibleRangeChanged();
protected:
    void startDrag(Qt::DropActions) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *e) override;
private:
    FileViewDnd *m_dnd;
    QTimer m_visibleTimer;
};

// Finder-style icon cell: thumbnail or native icon, name below, selection capsule.
class GridDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static constexpr int kCellWidth = 124;
    static constexpr int kCellHeight = 142;
    static constexpr int kIconBox = 96;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
};

// Row delegate for the list: dims secondary columns and adds row padding.
class ListDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void initStyleOption(QStyleOptionViewItem *opt, const QModelIndex &idx) const override;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
};
