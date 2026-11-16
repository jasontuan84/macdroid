#include "ui/Sidebar.h"

#include "app/Theme.h"
#include "platform/MacPlatform.h"
#include "ui/GlassWidgets.h"

#include <QMouseEvent>
#include "ui/Icons.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

namespace {

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() - 2);
    f.setWeight(QFont::DemiBold);
    l->setFont(f);
    l->setContentsMargins(10, 12, 0, 4);
    Theme::setSecondary(l, 0.55);
    return l;
}

} // namespace

// ------------------------------------------------------------------ SidebarItem

SidebarItem::SidebarItem(const QString &symbol, const QString &title, QWidget *parent)
    : QAbstractButton(parent)
    , m_symbol(symbol)
{
    setText(title);
    setAccessibleName(title);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
}

QSize SidebarItem::sizeHint() const
{
    return {200, m_usage >= 0 ? 44 : 30};
}

void SidebarItem::enterEvent(QEnterEvent *e)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(e);
}

void SidebarItem::leaveEvent(QEvent *e)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(e);
}

void SidebarItem::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool dark = Glass::isDark(this);
    if (m_selected || m_hover || isDown()) {
        p.setPen(Qt::NoPen);
        int a = m_selected ? (dark ? 40 : 22) : (dark ? 20 : 12);
        p.setBrush(dark ? QColor(255, 255, 255, a) : QColor(0, 0, 0, a));
        p.drawRoundedRect(QRectF(rect()).adjusted(0, 1, 0, -1), 8, 8);
    }
    const QColor ink = Glass::ink(this);
    const QColor accent = palette().color(QPalette::Accent);
    const QPixmap icon = Icons::symbol(m_symbol, 13, m_usage >= 0 ? ink : accent, devicePixelRatioF());
    const QSizeF is = icon.deviceIndependentSize();
    const qreal rowH = 30;
    p.drawPixmap(QPointF(10 + (18 - is.width()) / 2, (rowH - is.height()) / 2), icon);

    p.setPen(ink);
    QRectF tr(34, 0, width() - 44, rowH);
    QString detail = m_detail;
    if (!detail.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() - 2);
        p.setFont(f);
        p.setPen(Glass::secondaryInk(this));
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignRight, detail);
        tr.setRight(tr.right() - QFontMetrics(f).horizontalAdvance(detail) - 6);
        p.setFont(font());
        p.setPen(ink);
    }
    p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(font()).elidedText(text(), Qt::ElideRight, int(tr.width())));

    if (m_usage >= 0) {
        const QRectF bar(34, rowH + 2, width() - 44, 4);
        QColor track = ink;
        track.setAlphaF(0.12f);
        p.setPen(Qt::NoPen);
        p.setBrush(track);
        p.drawRoundedRect(bar, 2, 2);
        QColor fillc = m_usage > 0.9 ? QColor(255, 69, 58) : accent;
        p.setBrush(fillc);
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * qBound(0.0, m_usage, 1.0), bar.height()), 2, 2);
    }
}

// ------------------------------------------------------------------ Sidebar

const QStringList &Sidebar::favoriteFolderNames()
{
    static const QStringList names{"DCIM", "Download", "Pictures", "Documents", "Movies", "Music"};
    return names;
}

Sidebar::Sidebar(QWidget *parent)
    : QWidget(parent)
{
    setFixedWidth(236);
    // Empty sidebar space acts as title bar; double-click only next to the traffic lights.
    Glass::WindowMover::install(this, 44);
    setAttribute(Qt::WA_TranslucentBackground);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 44, 10, 10); // top: room for the traffic lights
    root->setSpacing(0);

    root->addWidget(sectionLabel(tr("Device"), this));

    m_deviceRow = new QWidget(this);
    auto *dl = new QHBoxLayout(m_deviceRow);
    dl->setContentsMargins(10, 2, 2, 2);
    dl->setSpacing(8);
    m_phoneIcon = new QLabel(m_deviceRow);
    m_phoneIcon->setPixmap(Icons::symbol(QStringLiteral("iphone"), 14, Glass::ink(this), 2.0));
    dl->addWidget(m_phoneIcon);
    connect(Theme::instance(), &Theme::changed, this, [this] {
        m_phoneIcon->setPixmap(Icons::symbol(QStringLiteral("iphone"), 14, Glass::ink(this), 2.0));
    });
    m_deviceName = new QLabel(tr("No device"), m_deviceRow);
    QFont bf = m_deviceName->font();
    bf.setWeight(QFont::DemiBold);
    m_deviceName->setFont(bf);
    dl->addWidget(m_deviceName, 1);
    auto *eject = new Glass::Button(QStringLiteral("eject"), tr("Eject"), m_deviceRow);
    eject->setFixedSize(28, 28);
    connect(eject, &QAbstractButton::clicked, this, &Sidebar::ejectClicked);
    dl->addWidget(eject);
    root->addWidget(m_deviceRow);

    m_storageBox = new QVBoxLayout();
    m_storageBox->setSpacing(2);
    root->addLayout(m_storageBox);

    m_favHeader = sectionLabel(tr("Favorites"), this);
    root->addWidget(m_favHeader);
    m_favBox = new QVBoxLayout();
    m_favBox->setSpacing(0);
    root->addLayout(m_favBox);
    root->addStretch(1);

    auto *statusRow = new QWidget(this);
    auto *sl = new QHBoxLayout(statusRow);
    sl->setContentsMargins(10, 6, 6, 6);
    sl->setSpacing(8);
    m_statusDot = new QWidget(statusRow);
    m_statusDot->setFixedSize(8, 8);
    sl->addWidget(m_statusDot);
    m_status = new QLabel(statusRow);
    QFont sf = m_status->font();
    sf.setPointSizeF(sf.pointSizeF() - 2);
    m_status->setFont(sf);
    sl->addWidget(m_status, 1);
    root->addWidget(statusRow);

    clearDevice(tr("No device connected"));
}

void Sidebar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool dark = Glass::isDark(this);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    // Floating glass panel: soft shadow, tinted fill, bright hairline.
    for (int i = 1; i <= 4; ++i) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, dark ? 14 : 5));
        p.drawRoundedRect(r.adjusted(-i * 0.5, -i * 0.25 + 1, i * 0.5, i * 0.75 + 1), 18 + i * 0.5, 18 + i * 0.5);
    }
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    if (dark) {
        g.setColorAt(0, QColor(70, 70, 76, 235));
        g.setColorAt(1, QColor(52, 52, 58, 235));
    } else {
        g.setColorAt(0, QColor(246, 246, 249, 245));
        g.setColorAt(1, QColor(236, 236, 241, 245));
    }
    p.setBrush(g);
    p.setPen(QPen(dark ? QColor(255, 255, 255, 36) : QColor(255, 255, 255, 230), 1));
    p.drawRoundedRect(r, 18, 18);
    p.setPen(QPen(dark ? QColor(0, 0, 0, 90) : QColor(0, 0, 0, 22), 0.5));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(-0.5, -0.5, 0.5, 0.5), 18.5, 18.5);

    // Status dot colour follows the connection state stored in the property.
    const QColor dot = m_statusDot->property("dotColor").value<QColor>();
    if (dot.isValid()) {
        const QPoint c = m_statusDot->mapTo(this, m_statusDot->rect().center());
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(QPointF(c) + QPointF(0.5, 0.5), 4, 4);
    }
}

void Sidebar::setDevice(const mtp::DeviceInfo &info)
{
    m_deviceName->setText(info.name);
    m_deviceRow->setVisible(true);
    QString status = tr("Connected via USB · MTP");
    if (info.batteryPercent >= 0)
        status += QStringLiteral(" · %1%").arg(info.batteryPercent);
    m_status->setText(status);
    m_statusDot->setProperty("dotColor", QColor(48, 209, 88));
    setStorages(info.storages);
    update();
}

void Sidebar::clearDevice(const QString &statusText)
{
    m_deviceName->setText(tr("No device"));
    m_deviceRow->setVisible(false);
    m_status->setText(statusText);
    m_statusDot->setProperty("dotColor", QColor(142, 142, 147));
    m_storages.clear();
    m_favNames.clear();
    rebuildStorages();
    rebuildFavorites();
    update();
}

void Sidebar::setStorages(const mtp::StorageList &storages)
{
    m_storages = storages;
    rebuildStorages();
}

void Sidebar::setFavorites(quint32 storageId, const QStringList &names)
{
    m_favStorage = storageId;
    m_favNames = names;
    rebuildFavorites();
}

void Sidebar::setCurrent(quint32 storageId, const QString &rootFolderName, bool atRoot)
{
    m_currentStorage = storageId;
    m_currentRootFolder = rootFolderName;
    m_atRoot = atRoot;
    for (int i = 0; i < m_storageItems.size() && i < m_storages.size(); ++i)
        m_storageItems[i]->setSelectedLook(m_storages[i].id == storageId && atRoot);
    for (int i = 0; i < m_favItems.size() && i < m_favNames.size(); ++i)
        m_favItems[i]->setSelectedLook(!atRoot && m_favStorage == storageId
                                       && m_favNames[i].compare(rootFolderName, Qt::CaseInsensitive) == 0);
}

void Sidebar::rebuildStorages()
{
    qDeleteAll(m_storageItems);
    m_storageItems.clear();
    const QLocale loc;
    for (const mtp::Storage &s : m_storages) {
        auto *item = new SidebarItem(s.removable ? QStringLiteral("sdcard") : QStringLiteral("internaldrive"), s.description, this);
        item->setDetail(tr("%1 free").arg(loc.formattedDataSize(s.freeBytes, 0, QLocale::DataSizeSIFormat)));
        if (s.capacity > 0)
            item->setUsage(1.0 - double(s.freeBytes) / double(s.capacity));
        const quint32 id = s.id;
        connect(item, &QAbstractButton::clicked, this, [this, id] { emit storageClicked(id); });
        m_storageBox->addWidget(item);
        m_storageItems << item;
    }
    setCurrent(m_currentStorage, m_currentRootFolder, m_atRoot);
}

void Sidebar::rebuildFavorites()
{
    qDeleteAll(m_favItems);
    m_favItems.clear();
    m_favHeader->setVisible(!m_favNames.isEmpty());
    for (const QString &name : m_favNames) {
        QString symbol = QStringLiteral("folder");
        if (name.compare(QLatin1String("DCIM"), Qt::CaseInsensitive) == 0) symbol = QStringLiteral("camera");
        else if (name.compare(QLatin1String("Download"), Qt::CaseInsensitive) == 0) symbol = QStringLiteral("arrow.down.circle");
        else if (name.compare(QLatin1String("Pictures"), Qt::CaseInsensitive) == 0) symbol = QStringLiteral("photo");
        else if (name.compare(QLatin1String("Documents"), Qt::CaseInsensitive) == 0) symbol = QStringLiteral("doc");
        else if (name.compare(QLatin1String("Movies"), Qt::CaseInsensitive) == 0) symbol = QStringLiteral("film");
        else if (name.compare(QLatin1String("Music"), Qt::CaseInsensitive) == 0) symbol = QStringLiteral("music.note");
        auto *item = new SidebarItem(symbol, name, this);
        const quint32 storage = m_favStorage;
        connect(item, &QAbstractButton::clicked, this, [this, storage, name] { emit favoriteClicked(storage, name); });
        m_favBox->addWidget(item);
        m_favItems << item;
    }
    setCurrent(m_currentStorage, m_currentRootFolder, m_atRoot);
}
