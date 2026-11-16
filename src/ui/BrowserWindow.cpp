#include "ui/BrowserWindow.h"

#include "app/Settings.h"
#include "app/Theme.h"
#include "core/MtpWorker.h"
#include "core/TransferManager.h"
#include "platform/MacPlatform.h"
#include "ui/FileModel.h"
#include "ui/FileViews.h"
#include "ui/GlassWidgets.h"
#include "ui/Icons.h"
#include "ui/Sidebar.h"
#include "ui/TransfersPopup.h"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QStyle>
#include <QStandardPaths>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <chrono>
#include <future>

namespace {

enum Page { ListPage = 0, GridPage = 1, PlaceholderPage = 2 };

QString cacheRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

mtp::ItemRef refFor(const mtp::Entry &e)
{
    return mtp::ItemRef{e.id, e.storageId, e.parentId, e.name, e.size, e.isFolder};
}

// Content background (window colour of the browser, like Finder's white area).
class ContentArea : public QWidget
{
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Base));
    }
};

} // namespace

BrowserWindow::BrowserWindow(MtpWorker *worker, TransferManager *transfers, QWidget *parent)
    : QMainWindow(parent)
    , m_worker(worker)
    , m_transfers(transfers)
{
    setWindowTitle(QStringLiteral("MacDroid"));
    setUnifiedTitleAndToolBarOnMac(false);
    setWindowFlag(Qt::ExpandedClientAreaHint, true);
    setWindowFlag(Qt::NoTitleBarBackgroundHint, true);
    resize(1080, 770); // default size (Finder-like proportions)
    setMinimumSize(940, 500);

    m_model = new FileModel(this);
    m_proxy = new FileSortProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setShowHidden(Settings::showHiddenFiles());

    buildUi();
    buildMenus();

    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(150);
    connect(&m_reloadTimer, &QTimer::timeout, this, &BrowserWindow::reload);
    m_loadingTimer.setSingleShot(true);
    m_loadingTimer.setInterval(300);
    connect(&m_loadingTimer, &QTimer::timeout, this, [this] {
        if (m_listReq && m_connected) {
            m_emptyLabel->setText(tr("Loading…"));
            m_emptyLabel->show();
        }
    });

    connect(m_worker, &MtpWorker::folderListed, this, &BrowserWindow::onFolderListed);
    connect(m_worker, &MtpWorker::pathResolved, this, &BrowserWindow::onPathResolved);
    connect(m_worker, &MtpWorker::operationFinished, this, &BrowserWindow::onOperationFinished);
    connect(m_worker, &MtpWorker::folderChanged, this, &BrowserWindow::onFolderChanged);
    connect(m_worker, &MtpWorker::thumbnailReady, m_model, &FileModel::setThumbnail);
    connect(m_model, &FileModel::renameRequested, this, [this](quint32 id, const QString &name) {
        const quint64 req = m_worker->rename(id, name);
        m_pendingOps.insert(req, name);
    });

    connect(m_transfers, &TransferManager::changed, this, [this] {
        const bool any = !m_transfers->transfers().isEmpty();
        m_progressBtn->setVisible(any);
        m_progressBtn->setProgress(m_transfers->overallFraction(), m_transfers->activeCount() > 0);
    });
    connect(m_transfers, &TransferManager::finished, this,
            [this](quint64, bool ok, bool cancelled, const QString &title, const QString &error) {
        if (!ok && !cancelled && !error.isEmpty())
            showError(title, error);
    });

    m_worker->setConflictResolver([this](const QString &name, bool isFolder, bool *all) {
        mtp::ConflictPolicy result = mtp::ConflictPolicy::Stop;
        QMetaObject::invokeMethod(this, [&] { result = askConflict(name, isFolder, all); }, Qt::BlockingQueuedConnection);
        return result;
    });

    setNoDevice();

    connect(Theme::instance(), &Theme::changed, this, [this] {
        if (m_stack->currentIndex() == PlaceholderPage)
            m_phIcon->setPixmap(Icons::symbol(m_connected || !m_phRetry->isHidden() ? QStringLiteral("exclamationmark.triangle") : QStringLiteral("iphone"),
                                              48, Glass::secondaryInk(this), devicePixelRatioF(), -1));
        update();
    });
}

// ------------------------------------------------------------------ UI construction

void BrowserWindow::buildUi()
{
    auto *central = new ContentArea(this);
    auto *h = new QHBoxLayout(central);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    // The sidebar floats 8 px inside the window edges (Liquid Glass style).
    auto *sideWrap = new QWidget(central);
    auto *sideLayout = new QVBoxLayout(sideWrap);
    sideLayout->setContentsMargins(8, 8, 6, 8);
    m_sidebar = new Sidebar(sideWrap);
    m_sidebar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    sideLayout->addWidget(m_sidebar);
    h->addWidget(sideWrap);
    m_sideWrap = sideWrap;
    connect(m_sidebar, &Sidebar::storageClicked, this, &BrowserWindow::navigateToStorage);
    connect(m_sidebar, &Sidebar::favoriteClicked, this, [this](quint32 storageId, const QString &name) {
        m_resolveReq = m_worker->resolvePath(storageId, {name});
    });
    connect(m_sidebar, &Sidebar::ejectClicked, this, &BrowserWindow::ejectRequested);

    auto *contentCol = new QWidget(central);
    auto *v = new QVBoxLayout(contentCol);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_toolbar = buildToolbar();
    v->addWidget(m_toolbar);

    m_stack = new QStackedWidget(contentCol);
    m_list = new FileListView(m_model, m_stack);
    m_grid = new FileGridView(m_model, m_stack);
    m_list->setModel(m_proxy);
    m_grid->setModel(m_proxy);
    m_grid->setSelectionModel(m_list->selectionModel());
    m_list->header()->setSectionResizeMode(FileModel::NameColumn, QHeaderView::Stretch);
    m_list->header()->resizeSection(FileModel::DateColumn, 180);
    m_list->header()->resizeSection(FileModel::SizeColumn, 90);
    m_list->header()->resizeSection(FileModel::KindColumn, 170);
    m_stack->addWidget(m_list);
    m_stack->addWidget(m_grid);
    m_stack->addWidget(buildPlaceholder());
    v->addWidget(m_stack, 1);

    m_emptyLabel = new QLabel(m_stack);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    Theme::setSecondary(m_emptyLabel, 0.45);
    m_emptyLabel->hide();

    m_pathBar = buildPathBar();
    v->addWidget(m_pathBar);
    h->addWidget(contentCol, 1);
    setCentralWidget(central);

    for (QAbstractItemView *view : {static_cast<QAbstractItemView *>(m_list), static_cast<QAbstractItemView *>(m_grid)}) {
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        view->installEventFilter(this);
        connect(view, &QWidget::customContextMenuRequested, this, &BrowserWindow::showContextMenu);
    }
    for (FileViewDnd *dnd : {m_list->dnd(), m_grid->dnd()}) {
        connect(dnd, &FileViewDnd::filesDropped, this, &BrowserWindow::handleFilesDropped);
        connect(dnd, &FileViewDnd::itemsDropped, this, &BrowserWindow::handleItemsDropped);
        connect(dnd, &FileViewDnd::startDragRequested, this, &BrowserWindow::startDrag);
        connect(dnd, &FileViewDnd::openRequested, this, [this](const QModelIndex &index) {
            const QModelIndex src = m_proxy->mapToSource(index);
            if (!src.isValid())
                return;
            // Keep a multi-selection when double-clicking one of its files; otherwise open just this item.
            const QList<int> sel = selectedSourceRows();
            if (sel.size() > 1 && sel.contains(src.row()) && !m_model->entry(src.row()).isFolder)
                openItems(sel);
            else
                openItems({src.row()});
        });
    }
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateActions();
        updateStatus();
    });
    connect(m_list->header(), &QHeaderView::sortIndicatorChanged, this, [this](int col, Qt::SortOrder order) {
        Settings::setSortColumn(col);
        Settings::setSortDescending(order == Qt::DescendingOrder);
        updateSortUi();
    });
    connect(m_grid, &FileGridView::visibleRangeChanged, this, &BrowserWindow::requestVisibleThumbnails);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this] { QTimer::singleShot(0, this, &BrowserWindow::requestVisibleThumbnails); });
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, [this] { QTimer::singleShot(0, this, &BrowserWindow::requestVisibleThumbnails); });

    m_list->sortByColumn(Settings::sortColumn(), Settings::sortDescending() ? Qt::DescendingOrder : Qt::AscendingOrder);
}

QWidget *BrowserWindow::buildToolbar()
{
    auto *bar = new Glass::DragArea(this);
    bar->setFixedHeight(56);
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(6, 10, 14, 10);
    h->setSpacing(10);

    auto *nav = new Glass::Group(bar);
    m_back = new Glass::Button(QStringLiteral("chevron.left"), tr("Back"), nav);
    m_forward = new Glass::Button(QStringLiteral("chevron.right"), tr("Forward"), nav);
    for (Glass::Button *b : {m_back, m_forward}) {
        b->setFixedSize(38, 34);
        nav->layout()->addWidget(b);
    }
    connect(m_back, &QAbstractButton::clicked, this, &BrowserWindow::goBack);
    connect(m_forward, &QAbstractButton::clicked, this, &BrowserWindow::goForward);
    h->addWidget(nav);

    m_title = new QLabel(bar);
    QFont tf = m_title->font();
    tf.setPointSizeF(tf.pointSizeF() + 2);
    tf.setWeight(QFont::Bold);
    m_title->setFont(tf);
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_title->setMinimumWidth(40);
    m_title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    h->addWidget(m_title, 0);
    h->addStretch(1);

    auto *views = new Glass::Group(bar, 3);
    m_gridBtn = new Glass::Button(QStringLiteral("square.grid.2x2"), tr("as Icons"), views);
    m_listBtn = new Glass::Button(QStringLiteral("list.bullet"), tr("as List"), views);
    for (Glass::Button *b : {m_gridBtn, m_listBtn}) {
        b->setCheckable(true);
        b->setFixedSize(34, 28);
        views->layout()->addWidget(b);
    }
    connect(m_gridBtn, &QAbstractButton::clicked, this, [this] { setViewMode(1); });
    connect(m_listBtn, &QAbstractButton::clicked, this, [this] { setViewMode(0); });
    h->addWidget(views);

    m_sortBtn = new Glass::Button(QStringLiteral("arrow.up.arrow.down"), tr("Sort By"), bar);
    m_sortBtn->setStandalone(true);
    m_sortBtn->setShowMenuArrow(true);
    m_sortBtn->setPopupMode(QToolButton::InstantPopup);
    h->addWidget(m_sortBtn);

    auto *actions = new Glass::Group(bar);
    m_newFolderBtn = new Glass::Button(QStringLiteral("folder.badge.plus"), tr("New Folder"), actions);
    m_copyToMacBtn = new Glass::Button(QStringLiteral("square.and.arrow.down"), tr("Copy to Mac…"), actions);
    m_deleteBtn = new Glass::Button(QStringLiteral("trash"), tr("Delete"), actions);
    for (Glass::Button *b : {m_newFolderBtn, m_copyToMacBtn, m_deleteBtn}) {
        b->setFixedSize(40, 34);
        actions->layout()->addWidget(b);
    }
    connect(m_newFolderBtn, &QAbstractButton::clicked, this, &BrowserWindow::newFolder);
    connect(m_copyToMacBtn, &QAbstractButton::clicked, this, &BrowserWindow::copyToMac);
    connect(m_deleteBtn, &QAbstractButton::clicked, this, &BrowserWindow::deleteSelection);
    h->addWidget(actions);

    m_progressBtn = new Glass::ProgressButton(bar);
    m_progressBtn->setToolTip(tr("Transfers"));
    m_progressBtn->setAccessibleName(tr("Transfers"));
    m_progressBtn->setFixedSize(34, 34);
    m_progressBtn->hide();
    connect(m_progressBtn, &QAbstractButton::clicked, this, [this] {
        if (!m_popup)
            m_popup = new TransfersPopup(m_transfers, this);
        m_popup->showBelow(m_progressBtn);
    });
    h->addWidget(m_progressBtn);

    m_search = new Glass::SearchField(bar);
    m_search->setPlaceholderText(tr("Search"));
    m_search->setFixedWidth(200);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_proxy->setSearchText(t);
        updateStatus();
        m_emptyLabel->setVisible(m_connected && m_proxy->rowCount() == 0 && !m_listReq);
        m_emptyLabel->setText(t.isEmpty() ? tr("This folder is empty") : tr("No results"));
    });
    h->addWidget(m_search);
    return bar;
}

QWidget *BrowserWindow::buildPathBar()
{
    auto *bar = new QWidget(this);
    bar->setFixedHeight(30);
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(16, 0, 16, 0);
    h->setSpacing(2);
    m_crumbs = new QHBoxLayout();
    m_crumbs->setSpacing(2);
    h->addLayout(m_crumbs);
    h->addStretch(1);
    m_status = new QLabel(bar);
    QFont f = m_status->font();
    f.setPointSizeF(f.pointSizeF() - 2);
    m_status->setFont(f);
    Theme::setSecondary(m_status, 0.6);
    h->addWidget(m_status);
    return bar;
}

QWidget *BrowserWindow::buildPlaceholder()
{
    m_placeholder = new QWidget(this);
    auto *v = new QVBoxLayout(m_placeholder);
    v->addStretch(1);
    m_phIcon = new QLabel(m_placeholder);
    m_phIcon->setAlignment(Qt::AlignCenter);
    v->addWidget(m_phIcon);
    m_phTitle = new QLabel(m_placeholder);
    m_phTitle->setAlignment(Qt::AlignCenter);
    QFont tf = m_phTitle->font();
    tf.setPointSizeF(tf.pointSizeF() + 5);
    tf.setWeight(QFont::Bold);
    m_phTitle->setFont(tf);
    v->addSpacing(8);
    v->addWidget(m_phTitle);
    m_phBody = new QLabel(m_placeholder);
    m_phBody->setAlignment(Qt::AlignCenter);
    m_phBody->setWordWrap(true);
    // A fixed width lets QLabel compute the wrapped height (no clipped lines).
    m_phBody->setFixedWidth(440);
    m_phBody->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::MinimumExpanding);
    Theme::setSecondary(m_phBody, 0.65);
    v->addSpacing(6);
    v->addWidget(m_phBody, 0, Qt::AlignHCenter);
    m_phRetry = new QPushButton(tr("Try Again"), m_placeholder);
    m_phRetry->setDefault(true);
    connect(m_phRetry, &QPushButton::clicked, this, [this] {
        if (m_heldByImageCapture)
            emit releaseRequested();
        else
            emit retryRequested();
    });
    v->addSpacing(12);
    v->addWidget(m_phRetry, 0, Qt::AlignHCenter);
    v->addStretch(2);
    // Without the toolbar, the empty page is where the window is dragged from.
    Glass::WindowMover::install(m_placeholder);
    return m_placeholder;
}

void BrowserWindow::buildMenus()
{
    auto add = [this](QMenu *menu, const QString &text, const QKeySequence &key, auto slot) {
        QAction *a = menu->addAction(text);
        if (!key.isEmpty())
            a->setShortcut(key);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };

    QMenu *file = menuBar()->addMenu(tr("File"));
    m_actNewFolder = add(file, tr("New Folder"), QKeySequence(QStringLiteral("Ctrl+Shift+N")), &BrowserWindow::newFolder);
    m_actOpen = add(file, tr("Open"), QKeySequence(QStringLiteral("Ctrl+Down")), &BrowserWindow::openSelection);
    file->addSeparator();
    m_actCopyToMac = add(file, tr("Copy to Mac…"), QKeySequence(QStringLiteral("Ctrl+Shift+E")), &BrowserWindow::copyToMac);
    m_actGetInfo = add(file, tr("Get Info"), QKeySequence(QStringLiteral("Ctrl+I")), &BrowserWindow::getInfo);
    file->addSeparator();
    QAction *close = file->addAction(tr("Close Window"));
    close->setShortcut(QKeySequence::Close);
    connect(close, &QAction::triggered, this, &QWidget::close);

    QMenu *edit = menuBar()->addMenu(tr("Edit"));
    m_actCut = add(edit, tr("Cut"), QKeySequence::Cut, [this] { copyToClipboard(true); });
    m_actCopy = add(edit, tr("Copy"), QKeySequence::Copy, [this] { copyToClipboard(false); });
    m_actPaste = add(edit, tr("Paste"), QKeySequence::Paste, [this] { pasteFromClipboard(false); });
    m_actMoveHere = add(edit, tr("Move Item Here"), QKeySequence(QStringLiteral("Ctrl+Alt+V")), [this] { pasteFromClipboard(true); });
    m_actDuplicate = add(edit, tr("Duplicate"), QKeySequence(QStringLiteral("Ctrl+D")), &BrowserWindow::duplicateSelection);
    edit->addSeparator();
    m_actRename = add(edit, tr("Rename"), QKeySequence(), &BrowserWindow::renameSelection);
    m_actDelete = add(edit, tr("Delete Permanently…"), QKeySequence(QStringLiteral("Ctrl+Backspace")), &BrowserWindow::deleteSelection);
    edit->addSeparator();
    m_actSelectAll = add(edit, tr("Select All"), QKeySequence::SelectAll, [this] { currentView()->selectAll(); });

    QMenu *view = menuBar()->addMenu(tr("View"));
    m_actIconView = add(view, tr("as Icons"), QKeySequence(QStringLiteral("Ctrl+1")), [this] { setViewMode(1); });
    m_actListView = add(view, tr("as List"), QKeySequence(QStringLiteral("Ctrl+2")), [this] { setViewMode(0); });
    m_actIconView->setCheckable(true);
    m_actListView->setCheckable(true);
    view->addSeparator();
    m_sortMenu = view->addMenu(tr("Sort By"));
    m_sortGroup = new QActionGroup(this);
    const QList<QPair<int, QString>> cols{{FileModel::NameColumn, tr("Name")}, {FileModel::KindColumn, tr("Kind")},
                                          {FileModel::DateColumn, tr("Date Modified")}, {FileModel::SizeColumn, tr("Size")}};
    for (const auto &c : cols) {
        QAction *a = m_sortMenu->addAction(c.second);
        a->setCheckable(true);
        a->setData(c.first);
        m_sortGroup->addAction(a);
        const int col = c.first;
        connect(a, &QAction::triggered, this, [this, col] {
            setSort(col, m_actDescending->isChecked() ? Qt::DescendingOrder : Qt::AscendingOrder);
        });
    }
    m_sortMenu->addSeparator();
    m_actDescending = m_sortMenu->addAction(tr("Descending"));
    m_actDescending->setCheckable(true);
    connect(m_actDescending, &QAction::triggered, this, [this](bool on) {
        setSort(m_proxy->sortColumn() < 0 ? 0 : m_proxy->sortColumn(), on ? Qt::DescendingOrder : Qt::AscendingOrder);
    });
    m_sortBtn->setMenu(m_sortMenu);
    view->addSeparator();
    m_actShowHidden = add(view, tr("Show Hidden Files"), QKeySequence(QStringLiteral("Ctrl+Shift+.")), [this](bool on) {
        Settings::setShowHiddenFiles(on);
        m_proxy->setShowHidden(on);
        updateStatus();
    });
    m_actShowHidden->setCheckable(true);
    Theme::instance()->populateMenu(view->addMenu(tr("Appearance")));
    m_actShowHidden->setChecked(Settings::showHiddenFiles());
    m_actTransfers = add(view, tr("Show Transfers"), QKeySequence(QStringLiteral("Ctrl+Alt+L")), [this] {
        if (!m_popup)
            m_popup = new TransfersPopup(m_transfers, this);
        m_progressBtn->show();
        m_popup->showBelow(m_progressBtn);
    });

    QMenu *go = menuBar()->addMenu(tr("Go"));
    m_actBack = add(go, tr("Back"), QKeySequence(QStringLiteral("Ctrl+[")), &BrowserWindow::goBack);
    m_actForward = add(go, tr("Forward"), QKeySequence(QStringLiteral("Ctrl+]")), &BrowserWindow::goForward);
    m_actUp = add(go, tr("Enclosing Folder"), QKeySequence(QStringLiteral("Ctrl+Up")), &BrowserWindow::goUp);
    go->addSeparator();
    for (const QString &name : Sidebar::favoriteFolderNames()) {
        QAction *a = go->addAction(name);
        connect(a, &QAction::triggered, this, [this, name] {
            if (m_connected && m_loc.isValid())
                m_resolveReq = m_worker->resolvePath(m_loc.storageId, {name});
        });
    }
    go->addSeparator();
    m_actRefresh = add(go, tr("Refresh"), QKeySequence(QStringLiteral("Ctrl+R")), &BrowserWindow::reload);

    QMenu *window = menuBar()->addMenu(tr("Window"));
    QAction *minimize = window->addAction(tr("Minimize"));
    minimize->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
    connect(minimize, &QAction::triggered, this, &QWidget::showMinimized);

    setViewMode(Settings::viewMode());
    updateSortUi();
}

// ------------------------------------------------------------------ connection state

void BrowserWindow::setChromeVisible(bool visible)
{
    // Not connected: a clean window with only the centred message.
    m_sideWrap->setVisible(visible);
    m_toolbar->setVisible(visible);
    m_pathBar->setVisible(visible);
}

void BrowserWindow::showPlaceholder(const QString &symbol, const QString &title, const QString &body, bool retry)
{
    m_phIcon->setPixmap(Icons::symbol(symbol, 48, Glass::secondaryInk(this), devicePixelRatioF(), -1));
    m_phTitle->setText(title);
    m_phBody->setText(body);
    // Size the wrapped text explicitly so every line is visible.
    const QRect needed = m_phBody->fontMetrics().boundingRect(QRect(0, 0, m_phBody->width(), 10000),
                                                              int(Qt::AlignCenter | Qt::TextWordWrap), body);
    m_phBody->setFixedHeight(needed.height() + 6);
    m_phRetry->setVisible(retry);
    m_stack->setCurrentIndex(PlaceholderPage);
    m_emptyLabel->hide();
}

void BrowserWindow::setNoDevice()
{
    m_connected = false;
    m_device = {};
    m_loc = {};
    m_backStack.clear();
    m_forwardStack.clear();
    m_model->clear();
    m_rootFolders.clear();
    m_title->setText(QStringLiteral("MacDroid"));
    m_sidebar->clearDevice(tr("No device connected"));
    setChromeVisible(false);
    showPlaceholder(QStringLiteral("iphone"), tr("Connect an Android phone"),
                    tr("Plug your phone into this Mac with a USB cable, unlock it, then choose "
                       "\"File transfer\" in the USB notification on the phone."), false);
    updateBreadcrumb();
    updateActions();
    updateStatus();
}

void BrowserWindow::setAttention(const QString &message, bool heldByImageCapture)
{
    m_heldByImageCapture = heldByImageCapture;
    m_phRetry->setText(heldByImageCapture ? tr("Release Phone") : tr("Try Again"));
    m_connected = false;
    m_model->clear();
    m_title->setText(QStringLiteral("MacDroid"));
    m_sidebar->clearDevice(tr("Waiting for permission"));
    setChromeVisible(false);
    if (heldByImageCapture)
        showPlaceholder(QStringLiteral("camera.badge.ellipsis"), tr("Image Capture is using your phone"), message, true);
    else
        showPlaceholder(QStringLiteral("exclamationmark.triangle"), tr("Allow access on your phone"),
                        message + QStringLiteral("\n\n")
                            + tr("1. Unlock your phone\n2. Tap the \"USB\" notification\n3. Choose \"File transfer\""),
                        true);
    updateBreadcrumb();
    updateActions();
    updateStatus();
}

void BrowserWindow::setDevice(const mtp::DeviceInfo &info)
{
    const bool sameDevice = m_connected && info.serial == m_device.serial;
    m_device = info;
    m_connected = true;
    m_sidebar->setDevice(info);
    setChromeVisible(true);
    m_stack->setCurrentIndex(Settings::viewMode() == 1 ? GridPage : ListPage);
    if (!sameDevice || !m_loc.isValid()) {
        m_backStack.clear();
        m_forwardStack.clear();
        if (!info.storages.isEmpty())
            navigateToStorage(info.storages.first().id);
    }
    updateActions();
}

void BrowserWindow::updateStorages(const mtp::StorageList &storages)
{
    m_device.storages = storages;
    if (m_connected)
        m_sidebar->setStorages(storages);
    updateStatus();
}

void BrowserWindow::openPath(const QStringList &parts)
{
    if (!m_connected || m_device.storages.isEmpty())
        return;
    const quint32 storage = m_loc.isValid() ? m_loc.storageId : m_device.storages.first().id;
    m_resolveReq = m_worker->resolvePath(storage, parts);
}

#ifndef NDEBUG
void BrowserWindow::loadDemo(int viewMode)
{
    mtp::DeviceInfo info;
    info.name = QStringLiteral("Pixel 8 Pro");
    info.serial = QStringLiteral("demo");
    info.batteryPercent = 82;
    info.storages = {mtp::Storage{1, QStringLiteral("Internal shared storage"), 128000000000LL, 41000000000LL, false}};
    m_device = info;
    m_connected = true;
    m_sidebar->setDevice(info);
    setChromeVisible(true);
    m_sidebar->setFavorites(1, {"DCIM", "Download", "Pictures", "Documents", "Movies", "Music"});
    m_loc.storageId = 1;
    m_loc.path = {mtp::PathNode{mtp::kRootId, info.storages.first().description}, mtp::PathNode{2, QStringLiteral("Download")}};
    m_sidebar->setCurrent(1, QStringLiteral("Download"), false);
    m_title->setText(QStringLiteral("Download"));
    const QDateTime now = QDateTime::currentDateTime();
    mtp::EntryList list;
    auto add = [&](quint32 id, const char *name, qint64 size, int daysAgo, bool folder) {
        mtp::Entry e;
        e.id = id;
        e.parentId = 2;
        e.storageId = 1;
        e.name = QString::fromUtf8(name);
        e.size = size;
        e.modified = now.addDays(-daysAgo).addSecs(-qint64(id) * 3600);
        e.isFolder = folder;
        list << e;
    };
    add(10, "Bluetooth", 0, 1, true);
    add(11, "Screenshots", 0, 0, true);
    add(12, "Telegram", 0, 4, true);
    add(20, "boarding-pass-HAN-OSL.pdf", 412000, 0, false);
    add(21, "IMG_20261112_184233.jpg", 4200000, 4, false);
    add(22, "invoice-2026-10.pdf", 128000, 14, false);
    add(23, "MacDroid-test.apk", 18600000, 18, false);
    add(24, "meeting-notes.docx", 56000, 19, false);
    add(25, "trip-video.mp4", 1240000000, 27, false);
    add(26, "budget.xlsx", 86300, 33, false);
    add(27, "wallpapers.zip", 86300000, 33, false);
    m_model->setEntries(1, 2, list);
    setViewMode(viewMode);
    m_stack->setCurrentIndex(viewMode == 1 ? GridPage : ListPage);
    selectNames({QStringLiteral("IMG_20261112_184233.jpg")}, false);
    updateBreadcrumb();
    updateActions();
    updateStatus();
}
#endif

// ------------------------------------------------------------------ navigation

QString BrowserWindow::storageName(quint32 storageId) const
{
    for (const mtp::Storage &s : m_device.storages)
        if (s.id == storageId)
            return s.description;
    return tr("Storage");
}

quint32 BrowserWindow::currentFolderId() const
{
    return m_loc.path.isEmpty() ? mtp::kRootId : m_loc.path.last().id;
}

void BrowserWindow::navigateToStorage(quint32 storageId)
{
    Location loc;
    loc.storageId = storageId;
    loc.path.append(mtp::PathNode{mtp::kRootId, storageName(storageId)});
    navigate(loc);
}

void BrowserWindow::navigate(const Location &loc, bool recordHistory)
{
    if (!m_connected)
        return;
    if (recordHistory && m_loc.isValid()) {
        m_backStack.append(m_loc);
        m_forwardStack.clear();
    }
    m_loc = loc;
    m_worker->cancelBackground();
    m_thumbRequested.clear();
    m_search->clear();
    m_model->setEntries(loc.storageId, currentFolderId(), {});
    m_title->setText(loc.path.last().name);
    setWindowTitle(loc.path.last().name);
    m_sidebar->setCurrent(loc.storageId, loc.path.size() > 1 ? loc.path.at(1).name : QString(), loc.path.size() == 1);
    updateBreadcrumb();
    updateActions();
    reload();
}

void BrowserWindow::reload()
{
    if (!m_connected || !m_loc.isValid())
        return;
    m_listReq = m_worker->listFolder(m_loc.storageId, currentFolderId());
    m_loadingTimer.start();
}

void BrowserWindow::goBack()
{
    if (m_backStack.isEmpty())
        return;
    m_forwardStack.append(m_loc);
    navigate(m_backStack.takeLast(), false);
}

void BrowserWindow::goForward()
{
    if (m_forwardStack.isEmpty())
        return;
    m_backStack.append(m_loc);
    navigate(m_forwardStack.takeLast(), false);
}

void BrowserWindow::goUp()
{
    if (m_loc.path.size() <= 1)
        return;
    Location up = m_loc;
    const QString childName = up.path.takeLast().name;
    navigate(up);
    m_selectAfterLoad = {childName};
}

void BrowserWindow::onFolderListed(quint64 req, quint32 storageId, quint32 parentId, const mtp::EntryList &entries,
                                   const QString &error)
{
    if (req != m_listReq)
        return;
    m_listReq = 0;
    m_loadingTimer.stop();
    if (!error.isEmpty()) {
        m_emptyLabel->hide();
        if (m_loc.path.size() > 1) {
            Location up = m_loc;
            up.path.removeLast();
            navigate(up, false);
        } else {
            m_status->setText(error);
        }
        return;
    }
    // Keep current selection across refreshes.
    QStringList keep = m_selectAfterLoad;
    if (keep.isEmpty() && parentId == m_model->parentId() && storageId == m_model->storageId()) {
        for (int row : selectedSourceRows())
            keep << m_model->entry(row).name;
    }
    m_model->setEntries(storageId, parentId, entries);
    if (parentId == mtp::kRootId) {
        QStringList folders;
        for (const mtp::Entry &e : entries)
            if (e.isFolder)
                folders << e.name;
        m_rootFolders.insert(storageId, folders);
    }
    // Favourites for this storage come from its root listing.
    if (m_rootFolders.contains(storageId)) {
        QStringList favs;
        const QStringList &root = m_rootFolders.value(storageId);
        for (const QString &name : Sidebar::favoriteFolderNames()) {
            for (const QString &r : root)
                if (r.compare(name, Qt::CaseInsensitive) == 0)
                    favs << r;
        }
        m_sidebar->setFavorites(storageId, favs);
        m_sidebar->setCurrent(m_loc.storageId, m_loc.path.size() > 1 ? m_loc.path.at(1).name : QString(), m_loc.path.size() == 1);
    } else if (parentId != mtp::kRootId) {
        // Fetch the root once to know which favourite folders exist.
        m_rootFolders.insert(storageId, {});
        const quint32 s = storageId;
        QMetaObject::Connection *conn = new QMetaObject::Connection;
        const quint64 rootReq = m_worker->listFolder(s, mtp::kRootId);
        *conn = connect(m_worker, &MtpWorker::folderListed, this,
                        [this, rootReq, conn](quint64 r, quint32 st, quint32, const mtp::EntryList &list, const QString &) {
            if (r != rootReq)
                return;
            disconnect(*conn);
            delete conn;
            QStringList folders, favs;
            for (const mtp::Entry &e : list)
                if (e.isFolder)
                    folders << e.name;
            m_rootFolders.insert(st, folders);
            for (const QString &name : Sidebar::favoriteFolderNames())
                for (const QString &f : folders)
                    if (f.compare(name, Qt::CaseInsensitive) == 0)
                        favs << f;
            m_sidebar->setFavorites(st, favs);
            m_sidebar->setCurrent(m_loc.storageId, m_loc.path.size() > 1 ? m_loc.path.at(1).name : QString(), m_loc.path.size() == 1);
        });
    }

    selectNames(keep, m_editAfterLoad);
    m_selectAfterLoad.clear();
    m_editAfterLoad = false;
    m_emptyLabel->setText(m_search->text().isEmpty() ? tr("This folder is empty") : tr("No results"));
    m_emptyLabel->setVisible(m_proxy->rowCount() == 0);
    m_emptyLabel->setGeometry(m_stack->rect());
    m_emptyLabel->raise();
    updateStatus();
    updateActions();
}

void BrowserWindow::onPathResolved(quint64 req, quint32 storageId, const mtp::Path &path, const QString &error)
{
    if (req != m_resolveReq)
        return;
    m_resolveReq = 0;
    if (!error.isEmpty()) {
        showError(tr("Folder not found"), error);
        return;
    }
    Location loc;
    loc.storageId = storageId;
    loc.path = path;
    loc.path.first().name = storageName(storageId);
    navigate(loc);
}

void BrowserWindow::onOperationFinished(quint64 req, bool ok, const QString &error, quint32)
{
    if (!m_pendingOps.contains(req))
        return;
    const QString name = m_pendingOps.take(req);
    const bool wasNewFolder = m_pendingNewFolders.remove(req);
    if (!ok) {
        showError(wasNewFolder ? tr("Could not create the folder") : tr("Could not rename the item"), error);
        reload();
        return;
    }
    m_selectAfterLoad = {name};
    m_editAfterLoad = wasNewFolder;
    reload();
}

void BrowserWindow::onFolderChanged(quint32 storageId, quint32 parentId)
{
    if (m_connected && m_loc.isValid() && storageId == m_loc.storageId && parentId == currentFolderId())
        m_reloadTimer.start();
}

// ------------------------------------------------------------------ helpers

QAbstractItemView *BrowserWindow::currentView() const
{
    return m_stack->currentIndex() == GridPage ? static_cast<QAbstractItemView *>(m_grid)
                                               : static_cast<QAbstractItemView *>(m_list);
}

QModelIndexList BrowserWindow::selectedProxyRows() const
{
    // One index (column 0) per selected row. The grid selects only column 0
    // while the list selects whole rows, so selectedRows() can't be used.
    QModelIndexList rows;
    QSet<int> seen;
    for (const QModelIndex &idx : m_list->selectionModel()->selectedIndexes()) {
        if (!seen.contains(idx.row())) {
            seen.insert(idx.row());
            rows << idx.sibling(idx.row(), 0);
        }
    }
    return rows;
}

QList<int> BrowserWindow::selectedSourceRows() const
{
    QList<int> rows;
    const QModelIndexList sel = selectedProxyRows();
    QList<QPair<int, int>> ordered; // proxy row -> source row, in visual order
    for (const QModelIndex &idx : sel)
        ordered.append({idx.row(), m_proxy->mapToSource(idx).row()});
    std::sort(ordered.begin(), ordered.end());
    for (const auto &p : ordered)
        rows << p.second;
    return rows;
}

mtp::ItemRefList BrowserWindow::selectedItems() const
{
    mtp::ItemRefList items;
    for (int row : selectedSourceRows())
        items << refFor(m_model->entry(row));
    return items;
}

void BrowserWindow::selectNames(const QStringList &names, bool edit)
{
    QItemSelectionModel *sm = m_list->selectionModel();
    sm->clearSelection();
    QModelIndex first;
    for (const QString &name : names) {
        const int row = m_model->rowForName(name);
        if (row < 0)
            continue;
        const QModelIndex idx = m_proxy->mapFromSource(m_model->index(row, 0));
        if (!idx.isValid())
            continue;
        sm->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (!first.isValid())
            first = idx;
    }
    if (first.isValid()) {
        sm->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
        currentView()->scrollTo(first);
        if (edit)
            currentView()->edit(first);
    }
}

void BrowserWindow::updateActions()
{
    const int selected = m_connected ? int(selectedProxyRows().size()) : 0;
    const bool ready = m_connected && m_loc.isValid();
    m_back->setEnabled(ready && !m_backStack.isEmpty());
    m_forward->setEnabled(ready && !m_forwardStack.isEmpty());
    m_actBack->setEnabled(m_back->isEnabled());
    m_actForward->setEnabled(m_forward->isEnabled());
    m_actUp->setEnabled(ready && m_loc.path.size() > 1);
    m_actRefresh->setEnabled(ready);
    m_actNewFolder->setEnabled(ready);
    m_newFolderBtn->setEnabled(ready);
    for (QAction *a : {m_actOpen, m_actCopyToMac, m_actGetInfo, m_actCopy, m_actCut, m_actDuplicate, m_actDelete})
        a->setEnabled(ready && selected > 0);
    m_actRename->setEnabled(ready && selected == 1);
    m_copyToMacBtn->setEnabled(ready && selected > 0);
    m_deleteBtn->setEnabled(ready && selected > 0);
    m_actPaste->setEnabled(ready);
    m_actMoveHere->setEnabled(ready);
    m_actSelectAll->setEnabled(ready);
    m_sortBtn->setEnabled(ready);
    m_search->setEnabled(ready);
    m_gridBtn->setEnabled(ready);
    m_listBtn->setEnabled(ready);
}

void BrowserWindow::updateStatus()
{
    if (!m_connected || !m_loc.isValid()) {
        m_status->clear();
        return;
    }
    const int total = m_proxy->rowCount();
    const int selected = int(selectedProxyRows().size());
    QString text = tr("%n item(s)", nullptr, total);
    if (selected > 0)
        text += QStringLiteral(", ") + tr("%n selected", nullptr, selected);
    for (const mtp::Storage &s : m_device.storages) {
        if (s.id == m_loc.storageId) {
            text += QStringLiteral(" · ") + tr("%1 available").arg(QLocale().formattedDataSize(s.freeBytes, 1, QLocale::DataSizeSIFormat));
            break;
        }
    }
    m_status->setText(text);
}

void BrowserWindow::updateBreadcrumb()
{
    while (QLayoutItem *item = m_crumbs->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (!m_connected || !m_loc.isValid())
        return;
    auto addCrumb = [this](const QString &text, int pathIndex, bool last) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        QFont f = b->font();
        f.setPointSizeF(f.pointSizeF() - 2);
        if (last)
            f.setWeight(QFont::DemiBold);
        b->setFont(f);
        b->setStyleSheet(QStringLiteral("QToolButton { border: none; padding: 2px 4px; }"));
        if (pathIndex >= 0 && !last) {
            connect(b, &QToolButton::clicked, this, [this, pathIndex] {
                Location loc = m_loc;
                loc.path.resize(pathIndex + 1);
                navigate(loc);
            });
        }
        m_crumbs->addWidget(b);
        if (!last) {
            auto *sep = new QLabel(QStringLiteral("›"), this);
            Theme::setSecondary(sep, 0.4);
            m_crumbs->addWidget(sep);
        }
    };
    addCrumb(m_device.name, -1, false);
    for (int i = 0; i < m_loc.path.size(); ++i)
        addCrumb(m_loc.path.at(i).name, i, i == m_loc.path.size() - 1);
}

void BrowserWindow::setViewMode(int mode)
{
    Settings::setViewMode(mode);
    m_gridBtn->setChecked(mode == 1);
    m_listBtn->setChecked(mode == 0);
    m_actIconView->setChecked(mode == 1);
    m_actListView->setChecked(mode == 0);
    if (m_stack->currentIndex() != PlaceholderPage) {
        m_stack->setCurrentIndex(mode == 1 ? GridPage : ListPage);
        currentView()->setFocus();
        const QModelIndex cur = m_list->selectionModel()->currentIndex();
        if (cur.isValid())
            currentView()->scrollTo(cur);
        QTimer::singleShot(0, this, &BrowserWindow::requestVisibleThumbnails);
    }
}

void BrowserWindow::setSort(int column, Qt::SortOrder order)
{
    m_list->sortByColumn(column, order);
}

void BrowserWindow::updateSortUi()
{
    if (!m_sortGroup)
        return; // menus not built yet
    const int col = m_proxy->sortColumn() < 0 ? 0 : m_proxy->sortColumn();
    for (QAction *a : m_sortGroup->actions()) {
        a->setChecked(a->data().toInt() == col);
        if (a->isChecked())
            m_sortBtn->setText(a->text());
    }
    m_actDescending->setChecked(m_proxy->sortOrder() == Qt::DescendingOrder);
    m_sortBtn->updateGeometry();
}

void BrowserWindow::requestVisibleThumbnails()
{
    if (!m_connected || m_stack->currentIndex() != GridPage)
        return;
    for (int row : m_grid->visibleSourceRows()) {
        if (row < 0 || row >= m_model->rowCount())
            continue;
        const mtp::Entry &e = m_model->entry(row);
        if (!e.hasThumbnail() || m_thumbRequested.contains(e.id) || m_model->hasThumbnail(e.id))
            continue;
        m_thumbRequested.insert(e.id);
        m_worker->thumbnail(e, 256);
    }
}

void BrowserWindow::showError(const QString &title, const QString &message)
{
    QMessageBox box(QMessageBox::Warning, title, title, QMessageBox::Ok, isVisible() ? this : nullptr);
    box.setInformativeText(message);
    box.exec();
}

mtp::ConflictPolicy BrowserWindow::askConflict(const QString &name, bool isFolder, bool *applyToAll)
{
    QMessageBox box(isVisible() ? this : nullptr);
    box.setIcon(QMessageBox::Warning);
    box.setText(isFolder ? tr("A folder named \"%1\" already exists in this location.").arg(name)
                         : tr("An item named \"%1\" already exists in this location.").arg(name));
    box.setInformativeText(tr("Do you want to replace it with the one you are copying?"));
    QPushButton *keepBoth = box.addButton(tr("Keep Both"), QMessageBox::AcceptRole);
    QPushButton *stop = box.addButton(tr("Stop"), QMessageBox::RejectRole);
    QPushButton *skip = box.addButton(tr("Skip"), QMessageBox::NoRole);
    QPushButton *replace = box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
    box.setDefaultButton(keepBoth);
    auto *all = new QCheckBox(tr("Apply to all"), &box);
    box.setCheckBox(all);
    box.exec();
    if (applyToAll)
        *applyToAll = all->isChecked();
    QAbstractButton *clicked = box.clickedButton();
    if (clicked == replace)
        return mtp::ConflictPolicy::Replace;
    if (clicked == skip)
        return mtp::ConflictPolicy::Skip;
    if (clicked == stop)
        return mtp::ConflictPolicy::Stop;
    return mtp::ConflictPolicy::KeepBoth;
}

QString BrowserWindow::openCacheDir() const
{
    const QString serial = m_device.serial.isEmpty() ? QStringLiteral("device") : m_device.serial;
    return cacheRoot() + QStringLiteral("/Open/") + serial;
}

// ------------------------------------------------------------------ actions

void BrowserWindow::openSelection()
{
    openItems(selectedSourceRows());
}

void BrowserWindow::openItems(const QList<int> &rows)
{
    if (rows.isEmpty() || !m_connected)
        return;
    // A single folder: navigate into it. Files: open with their default macOS app.
    if (rows.size() == 1 && m_model->entry(rows.first()).isFolder) {
        const mtp::Entry &e = m_model->entry(rows.first());
        Location loc = m_loc;
        loc.path.append(mtp::PathNode{e.id, e.name});
        navigate(loc);
        return;
    }
    for (int row : rows) {
        const mtp::Entry &e = m_model->entry(row);
        if (!e.isFolder)
            openFileWithDefaultApp(e);
    }
}

void BrowserWindow::openFileWithDefaultApp(const mtp::Entry &entry)
{
    // Each object gets its own cache folder so names never clash.
    const QString dir = openCacheDir() + u'/' + QString::number(entry.id);
    const QString path = dir + u'/' + entry.name;
    const QFileInfo fi(path);
    if (fi.exists() && fi.size() == entry.size) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
    QPointer<BrowserWindow> self(this);
    m_worker->download({refFor(entry)}, dir, mtp::ConflictPolicy::Replace,
                       [self, path](bool ok) {
                           if (!ok)
                               return;
                           QMetaObject::invokeMethod(qApp, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
                       },
                       tr("Opening \"%1\"").arg(entry.name));
    if (entry.size > 20 * 1000 * 1000)
        m_progressBtn->show();
}

void BrowserWindow::copyToMac()
{
    const mtp::ItemRefList items = selectedItems();
    if (items.isEmpty())
        return;
    const QString start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose where to copy the items"), start);
    if (dir.isEmpty())
        return;
    m_worker->download(items, dir, mtp::ConflictPolicy::Ask);
    m_progressBtn->show();
}

void BrowserWindow::newFolder()
{
    if (!m_connected || !m_loc.isValid())
        return;
    const QString name = mtp::uniqueName(tr("untitled folder"), [this](const QString &n) { return m_model->containsName(n); });
    const quint64 req = m_worker->createFolder(m_loc.storageId, currentFolderId(), name);
    m_pendingOps.insert(req, name);
    m_pendingNewFolders.insert(req);
}

void BrowserWindow::deleteSelection()
{
    const mtp::ItemRefList items = selectedItems();
    if (items.isEmpty())
        return;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setText(items.size() == 1 ? tr("Are you sure you want to delete \"%1\" permanently?").arg(items.first().name)
                                  : tr("Are you sure you want to delete these %n items permanently?", nullptr, int(items.size())));
    box.setInformativeText(tr("Phones connected over MTP have no Trash. You can't undo this action."));
    QPushButton *del = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != del)
        return;
    m_worker->remove(items);
}

void BrowserWindow::renameSelection()
{
    const QModelIndex cur = m_list->selectionModel()->currentIndex();
    if (cur.isValid())
        currentView()->edit(m_proxy->index(cur.row(), FileModel::NameColumn));
}

void BrowserWindow::getInfo()
{
    const QList<int> rows = selectedSourceRows();
    if (rows.isEmpty())
        return;
    const mtp::Entry &e = m_model->entry(rows.first());
    QStringList where;
    where << m_device.name;
    for (const mtp::PathNode &n : m_loc.path)
        where << n.name;
    const QLocale loc;
    QMessageBox box(this);
    box.setIconPixmap(Icons::filePixmap(e.extension(), e.isFolder, 64, devicePixelRatioF()));
    box.setText(e.name);
    box.setInformativeText(
        tr("Kind: %1").arg(Icons::kind(e.extension(), e.isFolder)) + u'\n'
        + (e.isFolder ? QString() : tr("Size: %1 (%2 bytes)").arg(loc.formattedDataSize(e.size, 1, QLocale::DataSizeSIFormat), loc.toString(e.size)) + u'\n')
        + tr("Where: %1").arg(where.join(QStringLiteral(" › "))) + u'\n'
        + tr("Modified: %1").arg(loc.toString(e.modified, QLocale::LongFormat)));
    box.exec();
}

void BrowserWindow::copyToClipboard(bool cut)
{
    const mtp::ItemRefList items = selectedItems();
    if (items.isEmpty())
        return;
    QVector<MacPlatform::PromisedItem> promised;
    for (const mtp::ItemRef &it : items)
        promised << MacPlatform::PromisedItem{it.name, it.isFolder};
    // Files are fetched only if another app (Finder) actually pastes them.
    m_clipboardStaging = cacheRoot() + QStringLiteral("/Clipboard/") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString staging = m_clipboardStaging;
    QPointer<BrowserWindow> self(this);
    MacPlatform::writeItemsToPasteboard(promised, mtp::encodeItems(m_device.serial, items, cut),
                                        [self, items, staging](int index) -> QString {
        if (!self || index < 0 || index >= items.size())
            return {};
        const mtp::ItemRef &it = items.at(index);
        const QString target = staging + u'/' + it.name;
        if (QFileInfo::exists(target))
            return target;
        QEventLoop loop;
        bool success = false;
        QMetaObject::Connection closed = connect(self->m_worker, &MtpWorker::deviceClosed, &loop, &QEventLoop::quit);
        self->m_worker->download({it}, staging, mtp::ConflictPolicy::Replace, [&loop, &success](bool ok) {
            success = ok;
            QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
        });
        loop.exec();
        QObject::disconnect(closed);
        return success ? target : QString();
    });
}

void BrowserWindow::pasteFromClipboard(bool move)
{
    if (!m_connected || !m_loc.isValid())
        return;
    if (MacPlatform::pasteboardHasInternalItems()) {
        QString serial;
        mtp::ItemRefList items;
        bool cut = false;
        mtp::decodeItems(MacPlatform::pasteboardInternalItems(), &serial, &items, &cut);
        if (items.isEmpty())
            return;
        if (serial != m_device.serial) {
            showError(tr("Can't paste"), tr("These items were copied from a different phone."));
            return;
        }
        if (move || cut) {
            if (items.first().parentId != currentFolderId() || items.first().storageId != m_loc.storageId)
                m_worker->moveOnDevice(items, m_loc.storageId, currentFolderId());
            if (cut)
                MacPlatform::clearPasteboard(); // a cut is pasted once, like Finder's move
        } else {
            m_worker->copyOnDevice(items, m_loc.storageId, currentFolderId());
        }
        return;
    }
    const QStringList paths = MacPlatform::pasteboardFilePaths();
    if (!paths.isEmpty()) {
        m_worker->upload(paths, m_loc.storageId, currentFolderId());
        m_progressBtn->show();
    }
}

void BrowserWindow::duplicateSelection()
{
    const mtp::ItemRefList items = selectedItems();
    if (!items.isEmpty())
        m_worker->copyOnDevice(items, m_loc.storageId, currentFolderId());
}

void BrowserWindow::startDrag()
{
    const mtp::ItemRefList items = selectedItems();
    if (items.isEmpty())
        return;
    QVector<MacPlatform::PromisedItem> promised;
    for (const mtp::ItemRef &it : items)
        promised << MacPlatform::PromisedItem{it.name, it.isFolder};

    // Drag image: thumbnail or icon of the first item, with a count badge.
    const qreal dpr = devicePixelRatioF();
    QImage preview(QSize(72, 72) * dpr, QImage::Format_ARGB32_Premultiplied);
    preview.setDevicePixelRatio(dpr);
    preview.fill(Qt::transparent);
    {
        QPainter p(&preview);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const mtp::Entry &first = m_model->entry(selectedSourceRows().first());
        const QVariant thumb = m_model->index(m_model->rowForId(first.id), 0).data(FileModel::ThumbnailRole);
        if (thumb.isValid()) {
            const QImage t = thumb.value<QImage>();
            const QSizeF s = QSizeF(t.size()).scaled(QSizeF(60, 60), Qt::KeepAspectRatio);
            p.drawImage(QRectF((72 - s.width()) / 2, (72 - s.height()) / 2, s.width(), s.height()), t);
        } else {
            p.drawPixmap(QPointF(4, 4), Icons::filePixmap(first.extension(), first.isFolder, 64, dpr));
        }
        if (items.size() > 1) {
            const QString n = QString::number(items.size());
            QFont f = font();
            f.setBold(true);
            f.setPointSizeF(11);
            p.setFont(f);
            const qreal w = qMax<qreal>(20, QFontMetricsF(f).horizontalAdvance(n) + 10);
            const QRectF badge(72 - w, 0, w, 20);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 59, 48));
            p.drawRoundedRect(badge, 10, 10);
            p.setPen(Qt::white);
            p.drawText(badge, Qt::AlignCenter, n);
        }
    }

    MtpWorker *worker = m_worker;
    const bool started = MacPlatform::startFilePromiseDrag(
        currentView()->viewport(), promised, mtp::encodeItems(m_device.serial, items), preview,
        // Runs on a background queue when Finder asks for the files.
        [worker, items](int index, const QString &destination) -> bool {
            if (index < 0 || index >= items.size())
                return false;
            auto promise = std::make_shared<std::promise<bool>>();
            std::future<bool> future = promise->get_future();
            worker->download({items.at(index)}, QFileInfo(destination).absolutePath(), mtp::ConflictPolicy::KeepBoth,
                             [promise](bool ok) { promise->set_value(ok); });
            while (future.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready) {
                if (worker->deviceSerial().isEmpty())
                    return false; // phone unplugged
            }
            return future.get();
        },
        [] {});
    Q_UNUSED(started);
}

void BrowserWindow::handleFilesDropped(const QStringList &paths, quint32 targetFolderId)
{
    if (!m_connected || !m_loc.isValid())
        return;
    m_worker->upload(paths, m_loc.storageId, targetFolderId ? targetFolderId : currentFolderId());
    m_progressBtn->show();
    MacPlatform::activateApp();
}

void BrowserWindow::handleItemsDropped(const QByteArray &data, quint32 targetFolderId, bool copy)
{
    QString serial;
    mtp::ItemRefList items;
    if (!mtp::decodeItems(data, &serial, &items) || items.isEmpty() || serial != m_device.serial)
        return;
    const quint32 target = targetFolderId ? targetFolderId : currentFolderId();
    for (const mtp::ItemRef &it : items)
        if (it.id == target)
            return; // dropped onto itself
    if (copy)
        m_worker->copyOnDevice(items, m_loc.storageId, target);
    else if (items.first().parentId != target)
        m_worker->moveOnDevice(items, m_loc.storageId, target);
}

void BrowserWindow::showContextMenu(const QPoint &pos)
{
    if (!m_connected || !m_loc.isValid())
        return;
    QAbstractItemView *view = currentView();
    const QModelIndex idx = view->indexAt(pos);
    QMenu menu(this);
    if (idx.isValid()) {
        if (!view->selectionModel()->isSelected(idx)) {
            view->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            view->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
        }
        const int n = int(selectedProxyRows().size());
        menu.addAction(m_actOpen);
        menu.addAction(m_actCopyToMac);
        menu.addSeparator();
        QAction *copy = menu.addAction(n == 1 ? tr("Copy \"%1\"").arg(m_model->entry(selectedSourceRows().first()).name)
                                              : tr("Copy %n Items", nullptr, n));
        copy->setShortcut(m_actCopy->shortcut());
        connect(copy, &QAction::triggered, this, [this] { copyToClipboard(false); });
        menu.addAction(m_actCut);
        menu.addAction(m_actDuplicate);
        menu.addSeparator();
        menu.addAction(m_actRename);
        menu.addAction(m_actGetInfo);
        menu.addSeparator();
        menu.addAction(m_actDelete);
    } else {
        view->clearSelection();
        menu.addAction(m_actNewFolder);
        menu.addSeparator();
        menu.addAction(m_actPaste);
        menu.addAction(m_actMoveHere);
        menu.addSeparator();
        menu.addAction(m_actIconView);
        menu.addAction(m_actListView);
        menu.addMenu(m_sortMenu);
        menu.addSeparator();
        menu.addAction(m_actRefresh);
    }
    menu.exec(view->viewport()->mapToGlobal(pos));
}

// ------------------------------------------------------------------ window events

bool BrowserWindow::eventFilter(QObject *watched, QEvent *event)
{
    // File shortcuts are handled here for the list/grid so the item views'
    // own text-copy handling can never swallow them.
    if (watched != m_list && watched != m_grid)
        return QMainWindow::eventFilter(watched, event);
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress)
        return QMainWindow::eventFilter(watched, event);
    auto *ke = static_cast<QKeyEvent *>(event);
    // (While renaming, keys go to the inline editor, not to the view, so
    // text editing shortcuts keep working.)

    std::function<void()> action;
    const QKeyCombination combo = ke->keyCombination();
    if (ke->matches(QKeySequence::Copy)) action = [this] { copyToClipboard(false); };
    else if (ke->matches(QKeySequence::Cut)) action = [this] { copyToClipboard(true); };
    else if (combo == QKeyCombination(Qt::ControlModifier | Qt::AltModifier, Qt::Key_V)) action = [this] { pasteFromClipboard(true); };
    else if (ke->matches(QKeySequence::Paste)) action = [this] { pasteFromClipboard(false); };
    else if (combo == QKeyCombination(Qt::ControlModifier, Qt::Key_Backspace)) action = [this] { deleteSelection(); };
    else if (combo == QKeyCombination(Qt::ControlModifier, Qt::Key_D)) action = [this] { duplicateSelection(); };
    else if (combo == QKeyCombination(Qt::ControlModifier, Qt::Key_Down) || combo == QKeyCombination(Qt::ControlModifier, Qt::Key_O)) action = [this] { openSelection(); };
    else if (combo == QKeyCombination(Qt::ControlModifier, Qt::Key_Up)) action = [this] { goUp(); };
    if (!action)
        return QMainWindow::eventFilter(watched, event);
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept(); // deliver as a key press to us instead of the menu shortcut
        return true;
    }
    action();
    return true;
}

void BrowserWindow::closeEvent(QCloseEvent *e)
{
    // The app lives in the menu bar: closing only hides the window.
    e->ignore();
    hide();
    emit windowHidden();
}

void BrowserWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    if (!m_styled) {
        m_styled = true;
        MacPlatform::styleBrowserWindow(this);
        MacPlatform::registerDropTypes(this);
        // First appearance: default size, centred on the screen with the pointer.
        if (QScreen *s = QGuiApplication::screenAt(QCursor::pos()) ? QGuiApplication::screenAt(QCursor::pos()) : screen()) {
            const QRect avail = s->availableGeometry();
            const QSize size(qMin(1080, avail.width() - 40), qMin(770, avail.height() - 40));
            setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size, avail));
        }
    }
    m_emptyLabel->setGeometry(m_stack->rect());
}

void BrowserWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange) {
        update();
        if (m_stack && m_stack->currentIndex() == PlaceholderPage)
            m_phIcon->setPixmap(Icons::symbol(m_connected ? QStringLiteral("exclamationmark.triangle") : QStringLiteral("iphone"),
                                              48, Glass::secondaryInk(this), devicePixelRatioF(), -1));
    }
}
