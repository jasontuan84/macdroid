#include "ui/FileViews.h"

#include "platform/MacPlatform.h"
#include "ui/FileModel.h"
#include "ui/Icons.h"

#include <QDragEnterEvent>
#include <QHeaderView>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QTextLayout>

namespace {

QModelIndex toSource(const QAbstractItemView *view, const QModelIndex &idx)
{
    if (auto *proxy = qobject_cast<const QSortFilterProxyModel *>(view->model()))
        return proxy->mapToSource(idx);
    return idx;
}

void paintDropFrame(QWidget *viewport)
{
    QPainter p(viewport);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor accent = viewport->palette().color(QPalette::Highlight);
    QColor bg = accent;
    bg.setAlpha(14);
    p.setBrush(bg);
    p.setPen(QPen(accent, 3));
    p.drawRoundedRect(QRectF(viewport->rect()).adjusted(2.5, 2.5, -2.5, -2.5), 12, 12);
}

} // namespace

// ------------------------------------------------------------------ DnD helper

FileViewDnd::FileViewDnd(QAbstractItemView *view, FileModel *model)
    : QObject(view)
    , m_view(view)
    , m_model(model)
{
}

bool FileViewDnd::acceptable(const QMimeData *md) const
{
    return MacPlatform::internalDragActive() || (md && md->hasUrls());
}

quint32 FileViewDnd::folderAt(const QPoint &pos) const
{
    const QModelIndex idx = m_view->indexAt(pos);
    if (!idx.isValid())
        return 0;
    const QModelIndex src = toSource(m_view, idx);
    const mtp::Entry &e = m_model->entry(src.row());
    if (!e.isFolder)
        return 0;
    // Do not offer a folder that is itself being dragged.
    if (MacPlatform::internalDragActive() && m_view->selectionModel()->isSelected(idx))
        return 0;
    return e.id;
}

void FileViewDnd::setTarget(quint32 id, bool active)
{
    m_targetId = id;
    m_active = active;
    m_model->setDropTarget(active ? id : 0);
    m_view->viewport()->update();
}

bool FileViewDnd::dragEnter(QDragEnterEvent *e)
{
    if (!acceptable(e->mimeData()))
        return false;
    e->setDropAction(MacPlatform::internalDragActive() && !(e->modifiers() & Qt::AltModifier) ? Qt::MoveAction : Qt::CopyAction);
    e->accept();
    setTarget(folderAt(e->position().toPoint()), true);
    return true;
}

bool FileViewDnd::dragMove(QDragMoveEvent *e)
{
    if (!acceptable(e->mimeData()))
        return false;
    const quint32 target = folderAt(e->position().toPoint());
    const bool internal = MacPlatform::internalDragActive();
    // Dropping internal items back onto the folder they came from is a no-op.
    if (internal && target == 0) {
        QString serial;
        mtp::ItemRefList items;
        mtp::decodeItems(MacPlatform::internalDragData(), &serial, &items);
        if (!items.isEmpty() && items.first().parentId == m_model->parentId()) {
            setTarget(0, false);
            e->ignore();
            return true;
        }
    }
    e->setDropAction(internal && !(e->modifiers() & Qt::AltModifier) ? Qt::MoveAction : Qt::CopyAction);
    e->accept();
    setTarget(target, true);
    return true;
}

void FileViewDnd::dragLeave()
{
    setTarget(0, false);
}

bool FileViewDnd::drop(QDropEvent *e)
{
    const quint32 target = folderAt(e->position().toPoint());
    setTarget(0, false);
    if (MacPlatform::internalDragActive()) {
        const bool copy = e->modifiers() & Qt::AltModifier;
        emit itemsDropped(MacPlatform::internalDragData(), target, copy);
        e->setDropAction(copy ? Qt::CopyAction : Qt::MoveAction);
        e->accept();
        return true;
    }
    if (e->mimeData() && e->mimeData()->hasUrls()) {
        QStringList paths;
        for (const QUrl &u : e->mimeData()->urls())
            if (u.isLocalFile())
                paths << u.toLocalFile();
        if (!paths.isEmpty()) {
            emit filesDropped(paths, target);
            e->setDropAction(Qt::CopyAction);
            e->accept();
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ List view

FileListView::FileListView(FileModel *model, QWidget *parent)
    : QTreeView(parent)
    , m_dnd(new FileViewDnd(this, model))
{
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setExpandsOnDoubleClick(false);
    setUniformRowHeights(true);
    setAlternatingRowColors(true);
    setSortingEnabled(true);
    setAllColumnsShowFocus(true);
    setSelectionMode(ExtendedSelection);
    setSelectionBehavior(SelectRows);
    setEditTriggers(EditKeyPressed | SelectedClicked);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(DragDrop);
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setIconSize(QSize(18, 18));
    setTextElideMode(Qt::ElideMiddle);
    setItemDelegate(new ListDelegate(this));
    header()->setStretchLastSection(false);
    header()->setSectionsMovable(false);
    header()->setHighlightSections(false);
    header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header()->setMinimumSectionSize(60);
}

void FileListView::startDrag(Qt::DropActions)
{
    emit m_dnd->startDragRequested();
}

void FileListView::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Finder behaviour: double-click opens the item under the pointer
    // (folder: navigate, file: open with the default app). Never edits.
    const QModelIndex idx = indexAt(e->position().toPoint());
    if (e->button() == Qt::LeftButton && idx.isValid()) {
        e->accept();
        m_dnd->requestOpen(idx);
        return;
    }
    QTreeView::mouseDoubleClickEvent(e);
}

void FileListView::dragEnterEvent(QDragEnterEvent *e)
{
    if (!m_dnd->dragEnter(e))
        e->ignore();
}

void FileListView::dragMoveEvent(QDragMoveEvent *e)
{
    if (!m_dnd->dragMove(e))
        e->ignore();
}

void FileListView::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dnd->dragLeave();
}

void FileListView::dropEvent(QDropEvent *e)
{
    if (!m_dnd->drop(e))
        e->ignore();
}

void FileListView::paintEvent(QPaintEvent *e)
{
    QTreeView::paintEvent(e);
    if (m_dnd->dropActive())
        paintDropFrame(viewport());
}

// ------------------------------------------------------------------ Grid view

FileGridView::FileGridView(FileModel *model, QWidget *parent)
    : QListView(parent)
    , m_dnd(new FileViewDnd(this, model))
{
    setViewMode(IconMode);
    setMovement(Static);
    setResizeMode(Adjust);
    setUniformItemSizes(true);
    setGridSize(QSize(GridDelegate::kCellWidth, GridDelegate::kCellHeight));
    setSpacing(0);
    setWordWrap(true);
    setSelectionMode(ExtendedSelection);
    setSelectionRectVisible(true);
    setEditTriggers(EditKeyPressed | SelectedClicked);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(DragDrop);
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setItemDelegate(new GridDelegate(this));
    setVerticalScrollMode(ScrollPerPixel);
    verticalScrollBar()->setSingleStep(24);
    viewport()->setAutoFillBackground(false);

    m_visibleTimer.setSingleShot(true);
    m_visibleTimer.setInterval(120);
    connect(&m_visibleTimer, &QTimer::timeout, this, &FileGridView::visibleRangeChanged);
}

QList<int> FileGridView::visibleSourceRows() const
{
    QList<int> rows;
    if (!model())
        return rows;
    const QRect vr = viewport()->rect().adjusted(0, -GridDelegate::kCellHeight, 0, GridDelegate::kCellHeight);
    const int count = model()->rowCount();
    for (int r = 0; r < count; ++r) {
        const QModelIndex idx = model()->index(r, 0);
        if (visualRect(idx).intersects(vr))
            rows << toSource(this, idx).row();
    }
    return rows;
}

void FileGridView::scrollContentsBy(int dx, int dy)
{
    QListView::scrollContentsBy(dx, dy);
    m_visibleTimer.start();
}

void FileGridView::resizeEvent(QResizeEvent *e)
{
    QListView::resizeEvent(e);
    m_visibleTimer.start();
}

void FileGridView::startDrag(Qt::DropActions)
{
    emit m_dnd->startDragRequested();
}

void FileGridView::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Finder behaviour: double-click opens the item under the pointer
    // (folder: navigate, file: open with the default app). Never edits.
    const QModelIndex idx = indexAt(e->position().toPoint());
    if (e->button() == Qt::LeftButton && idx.isValid()) {
        e->accept();
        m_dnd->requestOpen(idx);
        return;
    }
    QListView::mouseDoubleClickEvent(e);
}

void FileGridView::dragEnterEvent(QDragEnterEvent *e)
{
    if (!m_dnd->dragEnter(e))
        e->ignore();
}

void FileGridView::dragMoveEvent(QDragMoveEvent *e)
{
    if (!m_dnd->dragMove(e))
        e->ignore();
}

void FileGridView::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dnd->dragLeave();
}

void FileGridView::dropEvent(QDropEvent *e)
{
    if (!m_dnd->drop(e))
        e->ignore();
}

void FileGridView::paintEvent(QPaintEvent *e)
{
    QListView::paintEvent(e);
    if (m_dnd->dropActive())
        paintDropFrame(viewport());
}

// ------------------------------------------------------------------ Grid delegate

QSize GridDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return {kCellWidth, kCellHeight};
}

void GridDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);

    const QRect cell = opt.rect;
    const bool selected = opt.state & QStyle::State_Selected;
    const bool active = opt.state & QStyle::State_Active;
    const QPalette &pal = opt.palette;
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    const QRectF box(cell.center().x() - kIconBox / 2.0, cell.top() + 6, kIconBox, kIconBox);

    if (selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 22));
        p->drawRoundedRect(box.adjusted(-2, -2, 2, 2), 10, 10);
    }
    const QVariant dropBg = idx.data(Qt::BackgroundRole);
    if (dropBg.isValid()) {
        p->setPen(QPen(pal.color(QPalette::Highlight), 2));
        p->setBrush(dropBg.value<QColor>());
        p->drawRoundedRect(box.adjusted(-2, -2, 2, 2), 10, 10);
    }

    const QImage thumb = idx.data(FileModel::ThumbnailRole).value<QImage>();
    const QString ext = idx.data(FileModel::ExtensionRole).toString();
    const bool isFolder = idx.data(FileModel::IsFolderRole).toBool();
    if (!thumb.isNull()) {
        const QSizeF avail = box.size() - QSizeF(10, 10);
        QSizeF s = QSizeF(thumb.size()).scaled(avail, Qt::KeepAspectRatio);
        const QRectF tr(box.center().x() - s.width() / 2, box.bottom() - 5 - s.height(), s.width(), s.height());
        const bool isPdf = ext == QLatin1String("pdf");
        // Shadow + white frame like Finder's previews.
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 40));
        p->drawRoundedRect(tr.adjusted(-1.5, -0.5, 1.5, 2.5), 4, 4);
        p->setBrush(Qt::white);
        p->drawRoundedRect(tr.adjusted(-1.5, -1.5, 1.5, 1.5), 4, 4);
        QPainterPath clip;
        clip.addRoundedRect(tr, isPdf ? 1 : 3, isPdf ? 1 : 3);
        p->setClipPath(clip);
        p->drawImage(tr, thumb);
        p->setClipping(false);
        const bool isVideo = QStringList{"mp4", "mov", "m4v", "3gp", "mkv", "webm", "avi"}.contains(ext);
        if (isVideo) {
            const QPointF c = tr.center();
            p->setBrush(QColor(0, 0, 0, 120));
            p->drawEllipse(c, 13, 13);
            QPainterPath tri;
            tri.moveTo(c.x() - 4, c.y() - 7);
            tri.lineTo(c.x() + 7, c.y());
            tri.lineTo(c.x() - 4, c.y() + 7);
            tri.closeSubpath();
            p->fillPath(tri, Qt::white);
        }
    } else {
        const int iconSize = 72;
        const QPixmap pm = Icons::filePixmap(ext, isFolder, iconSize, p->device()->devicePixelRatioF());
        p->drawPixmap(QPointF(box.center().x() - iconSize / 2.0, box.bottom() - iconSize - 4), pm);
    }

    // Name: up to two lines, centred, middle-elided on the last line.
    QFont f = opt.font;
    f.setPointSizeF(f.pointSizeF() - 1);
    p->setFont(f);
    const QFontMetricsF fm(f);
    const QString name = idx.data(Qt::DisplayRole).toString();
    const qreal maxW = cell.width() - 10;
    QStringList lines;
    {
        QTextLayout layout(name, f);
        QTextOption to;
        to.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(to);
        layout.beginLayout();
        int consumed = 0;
        for (int i = 0; i < 2; ++i) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(maxW);
            if (i == 1) {
                lines << fm.elidedText(name.mid(consumed), Qt::ElideMiddle, maxW);
                break;
            }
            lines << name.mid(line.textStart(), line.textLength()).trimmed();
            consumed = line.textStart() + line.textLength();
        }
        layout.endLayout();
    }
    const qreal lh = fm.height();
    qreal y = box.bottom() + 6;
    for (const QString &line : lines) {
        const qreal w = fm.horizontalAdvance(line);
        const QRectF lr(cell.center().x() - w / 2, y, w, lh);
        if (selected) {
            p->setPen(Qt::NoPen);
            p->setBrush(active ? pal.color(QPalette::Highlight) : (dark ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 30)));
            p->drawRoundedRect(lr.adjusted(-5, 0, 5, 0), 4, 4);
            p->setPen(active ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Text));
        } else {
            p->setPen(pal.color(QPalette::Text));
        }
        p->drawText(lr, Qt::AlignCenter, line);
        y += lh;
    }
    p->restore();
}

QWidget *GridDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
{
    auto *ed = qobject_cast<QLineEdit *>(QStyledItemDelegate::createEditor(parent, opt, idx));
    if (ed) {
        ed->setAlignment(Qt::AlignCenter);
        // Select the name without the extension, like Finder.
        QMetaObject::invokeMethod(ed, [ed] {
            const QString t = ed->text();
            const int dot = t.lastIndexOf(u'.');
            if (dot > 0)
                ed->setSelection(0, dot);
        }, Qt::QueuedConnection);
    }
    return ed;
}

void GridDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &opt, const QModelIndex &) const
{
    const QRect cell = opt.rect;
    editor->setGeometry(cell.left() + 2, cell.top() + kIconBox + 10, cell.width() - 4, editor->sizeHint().height());
}

// ------------------------------------------------------------------ List delegate

void ListDelegate::initStyleOption(QStyleOptionViewItem *opt, const QModelIndex &idx) const
{
    QStyledItemDelegate::initStyleOption(opt, idx);
    if (idx.column() != FileModel::NameColumn && !(opt->state & QStyle::State_Selected)) {
        QColor c = opt->palette.color(QPalette::Text);
        c.setAlphaF(0.62f);
        opt->palette.setColor(QPalette::Text, c);
    }
}

QSize ListDelegate::sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const
{
    QSize s = QStyledItemDelegate::sizeHint(opt, idx);
    s.setHeight(qMax(s.height(), 26));
    return s;
}
