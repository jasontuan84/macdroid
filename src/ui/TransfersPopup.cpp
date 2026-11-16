#include "ui/TransfersPopup.h"

#include "app/Theme.h"
#include "core/TransferManager.h"
#include "ui/GlassWidgets.h"
#include "ui/Icons.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace {

class Bar : public QWidget
{
public:
    Bar(double fraction, bool failed, QWidget *parent)
        : QWidget(parent), m_fraction(fraction), m_failed(failed)
    {
        setFixedHeight(4);
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        QColor track = palette().color(QPalette::WindowText);
        track.setAlphaF(0.12f);
        p.setBrush(track);
        p.drawRoundedRect(QRectF(rect()), 2, 2);
        p.setBrush(m_failed ? QColor(255, 69, 58) : palette().color(QPalette::Highlight));
        p.drawRoundedRect(QRectF(0, 0, width() * qBound(0.0, m_fraction, 1.0), height()), 2, 2);
    }
private:
    double m_fraction;
    bool m_failed;
};

} // namespace

QString describeTransfer(qint64 done, qint64 total, double bytesPerSecond, qint64 secondsLeft)
{
    const QLocale loc;
    QString s = TransfersPopup::tr("%1 of %2").arg(loc.formattedDataSize(done, 1, QLocale::DataSizeSIFormat),
                                                    loc.formattedDataSize(total, 1, QLocale::DataSizeSIFormat));
    if (bytesPerSecond > 1)
        s += QStringLiteral(" · ") + TransfersPopup::tr("%1/s").arg(loc.formattedDataSize(qint64(bytesPerSecond), 1, QLocale::DataSizeSIFormat));
    if (secondsLeft >= 0) {
        if (secondsLeft < 60)
            s += QStringLiteral(" · ") + TransfersPopup::tr("about %n s", nullptr, int(qMax<qint64>(1, secondsLeft)));
        else
            s += QStringLiteral(" · ") + TransfersPopup::tr("about %n min", nullptr, int((secondsLeft + 59) / 60));
    }
    return s;
}

TransfersPopup::TransfersPopup(TransferManager *transfers, QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , m_transfers(transfers)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(360);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(12);

    auto *header = new QHBoxLayout();
    auto *title = new QLabel(tr("Transfers"), this);
    QFont tf = title->font();
    tf.setWeight(QFont::Bold);
    title->setFont(tf);
    header->addWidget(title, 1);
    auto *clear = new QPushButton(tr("Clear Finished"), this);
    clear->setFlat(true);
    clear->setCursor(Qt::PointingHandCursor);
    clear->setStyleSheet(QStringLiteral("QPushButton { border: none; color: palette(highlight); padding: 0; }"));
    connect(clear, &QPushButton::clicked, m_transfers, &TransferManager::clearFinished);
    header->addWidget(clear);
    root->addLayout(header);

    m_empty = new QLabel(tr("No transfers"), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setMinimumHeight(40);
    root->addWidget(m_empty);

    m_rows = new QVBoxLayout();
    m_rows->setSpacing(12);
    root->addLayout(m_rows);

    connect(m_transfers, &TransferManager::changed, this, [this] {
        if (isVisible())
            rebuild();
    });
}

void TransfersPopup::showBelow(QWidget *anchor)
{
    rebuild();
    adjustSize();
    const QPoint g = anchor->mapToGlobal(QPoint(anchor->width() / 2, anchor->height() + 6));
    QPoint pos(g.x() - width() / 2, g.y());
    if (QScreen *s = anchor->screen()) {
        const QRect avail = s->availableGeometry();
        pos.setX(qBound(avail.left() + 8, pos.x(), avail.right() - width() - 8));
    }
    move(pos);
    show();
}

void TransfersPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool dark = Glass::isDark(this);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    p.setPen(QPen(dark ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 40), 0.5));
    p.setBrush(dark ? QColor(44, 44, 48, 245) : QColor(250, 250, 252, 248));
    p.drawRoundedRect(r, 18, 18);
}

void TransfersPopup::rebuild()
{
    while (QLayoutItem *item = m_rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto &list = m_transfers->transfers();
    m_empty->setVisible(list.isEmpty());
    int shown = 0;
    for (const TransferManager::Transfer &t : list) {
        if (++shown > 8)
            break;
        auto *row = new QWidget(this);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);

        const bool up = t.kind == 0; // Upload
        auto *icon = new QLabel(row);
        icon->setFixedSize(32, 32);
        icon->setAlignment(Qt::AlignCenter);
        const QString sym = t.state == TransferManager::State::Failed ? QStringLiteral("exclamationmark.triangle")
                          : t.state == TransferManager::State::Done   ? QStringLiteral("checkmark")
                          : up ? QStringLiteral("arrow.up") : QStringLiteral("arrow.down");
        icon->setPixmap(Icons::symbol(sym, 13, palette().color(QPalette::Highlight), devicePixelRatioF(), 1));
        icon->setStyleSheet(QStringLiteral("background: rgba(10,122,255,0.14); border-radius: 8px;"));
        h->addWidget(icon);

        auto *col = new QVBoxLayout();
        col->setSpacing(4);
        auto *top = new QHBoxLayout();
        auto *title = new QLabel(row);
        title->setText(title->fontMetrics().elidedText(t.title, Qt::ElideMiddle, 210));
        top->addWidget(title, 1);
        auto *pct = new QLabel(row);
        if (t.state == TransferManager::State::Running && t.totalBytes > 0)
            pct->setText(QStringLiteral("%1%").arg(int(t.fraction() * 100)));
        top->addWidget(pct);
        col->addLayout(top);
        col->addWidget(new Bar(t.state == TransferManager::State::Done ? 1.0 : t.fraction(),
                               t.state == TransferManager::State::Failed, row));
        auto *detail = new QLabel(row);
        QFont df = detail->font();
        df.setPointSizeF(df.pointSizeF() - 2);
        detail->setFont(df);
        Theme::setSecondary(detail, 0.6);
        switch (t.state) {
        case TransferManager::State::Running:
            detail->setText(t.totalItems > 1
                ? tr("%1 of %2 items").arg(t.doneItems).arg(t.totalItems) + QStringLiteral(" · ")
                      + describeTransfer(t.doneBytes, t.totalBytes, t.bytesPerSecond, t.secondsLeft())
                : describeTransfer(t.doneBytes, t.totalBytes, t.bytesPerSecond, t.secondsLeft()));
            break;
        case TransferManager::State::Done:
            detail->setText(tr("Completed"));
            break;
        case TransferManager::State::Cancelled:
            detail->setText(tr("Cancelled"));
            break;
        case TransferManager::State::Failed:
            detail->setText(t.error.section(u'\n', 0, 0));
            detail->setToolTip(t.error);
            break;
        }
        col->addWidget(detail);
        h->addLayout(col, 1);

        if (t.state == TransferManager::State::Running) {
            auto *cancel = new Glass::Button(QStringLiteral("xmark"), tr("Cancel"), row);
            cancel->setFixedSize(26, 26);
            const quint64 id = t.id;
            connect(cancel, &QAbstractButton::clicked, m_transfers, [this, id] { m_transfers->cancel(id); });
            h->addWidget(cancel);
        }
        m_rows->addWidget(row);
    }
    adjustSize();
}
