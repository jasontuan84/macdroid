#pragma once

#include "core/MtpTypes.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QTimer>

class MtpWorker;
class TransferManager;
class FileModel;
class FileSortProxy;
class FileListView;
class FileGridView;
class Sidebar;
class TransfersPopup;
class QAbstractItemView;
class QStackedWidget;
class QLabel;
class QHBoxLayout;
class QPushButton;
class QAction;
class QActionGroup;
class QMenu;
namespace Glass { class Button; class ProgressButton; class SearchField; }

// The Finder-like browser window for the connected phone.
class BrowserWindow : public QMainWindow
{
    Q_OBJECT
public:
    BrowserWindow(MtpWorker *worker, TransferManager *transfers, QWidget *parent = nullptr);

    void setNoDevice();
    void setAttention(const QString &message, bool heldByImageCapture = false);
    void setDevice(const mtp::DeviceInfo &info);
    void updateStorages(const mtp::StorageList &storages);
    // Opens a folder by path from the first storage, e.g. {"DCIM", "Camera"}.
    void openPath(const QStringList &parts);
#ifndef NDEBUG
    // Debug builds only: fills the window with sample data for UI work without a phone.
    void loadDemo(int viewMode);
#endif

signals:
    void windowHidden();
    void retryRequested();
    void releaseRequested();
    void ejectRequested();

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    struct Location {
        quint32 storageId = 0;
        mtp::Path path;   // path.first() is the storage root
        bool isValid() const { return storageId != 0 && !path.isEmpty(); }
    };

    void buildUi();
    void buildMenus();
    QWidget *buildToolbar();
    QWidget *buildPathBar();
    QWidget *buildPlaceholder();

    // navigation
    void navigate(const Location &loc, bool recordHistory = true);
    void navigateToStorage(quint32 storageId);
    void goBack();
    void goForward();
    void goUp();
    void reload();
    quint32 currentFolderId() const;
    QString storageName(quint32 storageId) const;
    void setChromeVisible(bool visible);
    void showPlaceholder(const QString &symbol, const QString &title, const QString &body, bool retry);

    // results from the worker
    void onFolderListed(quint64 req, quint32 storageId, quint32 parentId, const mtp::EntryList &entries, const QString &error);
    void onPathResolved(quint64 req, quint32 storageId, const mtp::Path &path, const QString &error);
    void onOperationFinished(quint64 req, bool ok, const QString &error, quint32 newId);
    void onFolderChanged(quint32 storageId, quint32 parentId);

    // selection & actions
    QAbstractItemView *currentView() const;
    QModelIndexList selectedProxyRows() const;
    QList<int> selectedSourceRows() const;
    mtp::ItemRefList selectedItems() const;
    void selectNames(const QStringList &names, bool edit);
    void updateActions();
    void updateStatus();
    void updateBreadcrumb();
    void updateSortUi();
    void setViewMode(int mode);
    void setSort(int column, Qt::SortOrder order);
    void requestVisibleThumbnails();

    void openSelection();
    void openItems(const QList<int> &rows);
    void openFileWithDefaultApp(const mtp::Entry &entry);
    void copyToMac();
    void newFolder();
    void deleteSelection();
    void renameSelection();
    void getInfo();
    void copyToClipboard(bool cut = false);
    void pasteFromClipboard(bool move);
    void duplicateSelection();
    void startDrag();
    void handleFilesDropped(const QStringList &paths, quint32 targetFolderId);
    void handleItemsDropped(const QByteArray &data, quint32 targetFolderId, bool copy);
    void showContextMenu(const QPoint &viewportPos);
    void showError(const QString &title, const QString &message);
    mtp::ConflictPolicy askConflict(const QString &name, bool isFolder, bool *applyToAll);
    QString openCacheDir() const;

    MtpWorker *m_worker;
    TransferManager *m_transfers;
    FileModel *m_model;
    FileSortProxy *m_proxy;
    FileListView *m_list;
    FileGridView *m_grid;
    Sidebar *m_sidebar;
    TransfersPopup *m_popup = nullptr;
    QStackedWidget *m_stack;
    QLabel *m_emptyLabel;
    QLabel *m_title;
    QLabel *m_status;
    QHBoxLayout *m_crumbs;
    QWidget *m_placeholder;
    QWidget *m_sideWrap = nullptr;
    QWidget *m_toolbar = nullptr;
    QWidget *m_pathBar = nullptr;
    QLabel *m_phIcon;
    QLabel *m_phTitle;
    QLabel *m_phBody;
    QPushButton *m_phRetry;
    Glass::Button *m_back;
    Glass::Button *m_forward;
    Glass::Button *m_gridBtn;
    Glass::Button *m_listBtn;
    Glass::Button *m_sortBtn;
    Glass::Button *m_newFolderBtn;
    Glass::Button *m_copyToMacBtn;
    Glass::Button *m_deleteBtn;
    Glass::ProgressButton *m_progressBtn;
    Glass::SearchField *m_search;

    QAction *m_actOpen = nullptr, *m_actCopyToMac = nullptr, *m_actNewFolder = nullptr, *m_actGetInfo = nullptr,
        *m_actCopy = nullptr, *m_actCut = nullptr, *m_actPaste = nullptr, *m_actMoveHere = nullptr, *m_actDuplicate = nullptr,
        *m_actRename = nullptr, *m_actDelete = nullptr, *m_actSelectAll = nullptr, *m_actIconView = nullptr,
        *m_actListView = nullptr, *m_actShowHidden = nullptr, *m_actBack = nullptr, *m_actForward = nullptr,
        *m_actUp = nullptr, *m_actRefresh = nullptr, *m_actTransfers = nullptr, *m_actDescending = nullptr;
    QActionGroup *m_sortGroup = nullptr;
    QMenu *m_sortMenu = nullptr;

    mtp::DeviceInfo m_device;
    bool m_connected = false;
    Location m_loc;
    QVector<Location> m_backStack;
    QVector<Location> m_forwardStack;
    quint64 m_listReq = 0;
    quint64 m_resolveReq = 0;
    QHash<quint64, QString> m_pendingOps;     // request -> name to select afterwards
    QSet<quint64> m_pendingNewFolders;
    QStringList m_selectAfterLoad;
    bool m_editAfterLoad = false;
    QSet<quint32> m_thumbRequested;
    QHash<quint32, QStringList> m_rootFolders; // storage -> root folder names
    QTimer m_reloadTimer;
    QTimer m_loadingTimer;
    QString m_clipboardStaging;
    bool m_styled = false;
    bool m_heldByImageCapture = false;
};
