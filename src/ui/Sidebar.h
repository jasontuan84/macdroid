#pragma once

#include "core/MtpTypes.h"

#include <QAbstractButton>
#include <QWidget>

class QVBoxLayout;
class QLabel;

// One clickable sidebar row: SF Symbol + title (+ optional detail and capacity bar).
class SidebarItem : public QAbstractButton
{
    Q_OBJECT
public:
    SidebarItem(const QString &symbol, const QString &title, QWidget *parent = nullptr);
    void setDetail(const QString &detail) { m_detail = detail; update(); }
    void setUsage(double fraction) { m_usage = fraction; updateGeometry(); update(); }
    void setSelectedLook(bool on) { m_selected = on; update(); }
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    QString m_symbol;
    QString m_detail;
    double m_usage = -1;
    bool m_selected = false;
    bool m_hover = false;
};

// Floating glass sidebar: device, storages, favourite folders, status.
class Sidebar : public QWidget
{
    Q_OBJECT
public:
    explicit Sidebar(QWidget *parent = nullptr);

    void setDevice(const mtp::DeviceInfo &info);
    void clearDevice(const QString &statusText);
    void setStorages(const mtp::StorageList &storages);
    void setFavorites(quint32 storageId, const QStringList &names);
    void setCurrent(quint32 storageId, const QString &rootFolderName, bool atRoot);

    static const QStringList &favoriteFolderNames();

signals:
    void storageClicked(quint32 storageId);
    void favoriteClicked(quint32 storageId, const QString &name);
    void ejectClicked();

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void rebuildStorages();
    void rebuildFavorites();

    QVBoxLayout *m_storageBox;
    QVBoxLayout *m_favBox;
    QLabel *m_deviceName;
    QLabel *m_phoneIcon;
    QWidget *m_deviceRow;
    QLabel *m_favHeader;
    QLabel *m_status;
    QWidget *m_statusDot;
    mtp::StorageList m_storages;
    quint32 m_favStorage = 0;
    QStringList m_favNames;
    quint32 m_currentStorage = 0;
    QString m_currentRootFolder;
    bool m_atRoot = true;
    QList<SidebarItem *> m_storageItems;
    QList<SidebarItem *> m_favItems;
};
