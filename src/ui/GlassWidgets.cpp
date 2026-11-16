#include "ui/GlassWidgets.h"

#include "platform/MacPlatform.h"
#include "ui/Icons.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionToolButton>
#include <QWindow>

namespace Glass {

bool isDark(const QWidget *w)
{
    return w->palette().color(QPalette::Window).lightness() < 128;
}

QColor fill(const QWidget *w, bool pressed)
{
    if (isDark(w))
        return pressed ? QColor(255, 255, 255, 46) : QColor(255, 255, 255, 26);
    return pressed ? QColor(0, 0, 0, 22) : QColor(255, 255, 255, 200);
}

QColor border(const QWidget *w)
{
    return isDark(w) ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 30);
}

QColor ink(const QWidget *w, bool enabled)
{
    QColor c = w->palette().color(QPalette::WindowText);
    if (!enabled)
        c.setAlphaF(0.3f);
    return c;
}

QColor secondaryInk(const QWidget *w)
{
    QColor c = w->palette().color(QPalette::WindowText);
    c.setAlphaF(0.62f);
    return c;
}

void paintCapsule(QPainter &p, const QRectF &r, const QWidget *w, bool pressed)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const qreal radius = r.height() / 2.0;
    // Soft drop shadow
    if (!isDark(w)) {
        for (int i = 1; i <= 3; ++i) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 6));
            p.drawRoundedRect(r.adjusted(-i * 0.5, -i * 0.2 + 1, i * 0.5, i * 0.6 + 1), radius + i * 0.5, radius + i * 0.5);
        }
    }
    p.setBrush(fill(w, pressed));
    p.setPen(QPen(border(w), 0.5));
    p.drawRoundedRect(r.adjusted(0.25, 0.25, -0.25, -0.25), radius, radius);
    // Specular highlight along the top edge (glass)
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0, QColor(255, 255, 255, isDark(w) ? 30 : 140));
    g.setColorAt(0.5, QColor(255, 255, 255, 0));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(g), 1));
    p.drawRoundedRect(r.adjusted(0.75, 0.75, -0.75, -0.75), radius - 0.5, radius - 0.5);
    p.restore();
}

// ------------------------------------------------------------------ Group

Group::Group(QWidget *parent, int padding)
    : QWidget(parent)
{
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(padding, padding, padding, padding);
    l->setSpacing(0);
    setAttribute(Qt::WA_TranslucentBackground);
}

void Group::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    paintCapsule(p, QRectF(rect()), this);
}

// ------------------------------------------------------------------ Button

Button::Button(const QString &symbol, const QString &accessibleName, QWidget *parent)
    : QToolButton(parent)
    , m_symbol(symbol)
{
    setAccessibleName(accessibleName);
    setToolTip(accessibleName);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_Hover);
}

void Button::setSymbol(const QString &symbol)
{
    m_symbol = symbol;
    update();
}

QSize Button::sizeHint() const
{
    int w = 38;
    if (!text().isEmpty()) {
        // Matches paintEvent: 12 pad + icon (~18) + 6 gap + text + arrow area + 12 pad.
        w = 12 + 18 + 6 + fontMetrics().horizontalAdvance(text()) + (m_menuArrow ? 8 + 10 : 0) + 12;
    }
    return {w, 34};
}

void Button::enterEvent(QEnterEvent *e)
{
    m_hover = true;
    update();
    QToolButton::enterEvent(e);
}

void Button::leaveEvent(QEvent *e)
{
    m_hover = false;
    update();
    QToolButton::leaveEvent(e);
}

void Button::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0, 0, 0, 0);
    const bool down = isDown() || (isCheckable() && isChecked());
    if (m_standalone) {
        paintCapsule(p, r, this, isDown());
        if (isCheckable() && isChecked()) {
            p.setPen(Qt::NoPen);
            p.setBrush(isDark(this) ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 22));
            p.drawRoundedRect(r.adjusted(3, 3, -3, -3), (r.height() - 6) / 2, (r.height() - 6) / 2);
        }
    } else if (down || (m_hover && isEnabled())) {
        p.setPen(Qt::NoPen);
        QColor c = isDark(this) ? QColor(255, 255, 255, down ? 40 : 20) : QColor(0, 0, 0, down ? 24 : 12);
        p.setBrush(c);
        p.drawRoundedRect(r.adjusted(3, 3, -3, -3), (r.height() - 6) / 2, (r.height() - 6) / 2);
    }

    const qreal dpr = devicePixelRatioF();
    const QColor color = ink(this, isEnabled());
    const QPixmap icon = Icons::symbol(m_symbol, 14, color, dpr);
    const QSizeF isz = icon.deviceIndependentSize();
    qreal x = text().isEmpty() ? (width() - isz.width()) / 2 : 12;
    p.drawPixmap(QPointF(x, (height() - isz.height()) / 2), icon);
    if (!text().isEmpty()) {
        p.setPen(color);
        const QRectF tr(x + isz.width() + 6, 0, width() - x - isz.width() - 6, height());
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, text());
        if (m_menuArrow) {
            const QPixmap chev = Icons::symbol(QStringLiteral("chevron.down"), 9, secondaryInk(this), dpr, 1);
            const QSizeF cs = chev.deviceIndependentSize();
            p.drawPixmap(QPointF(width() - cs.width() - 12, (height() - cs.height()) / 2), chev);
        }
    }
}

// ------------------------------------------------------------------ ProgressButton

ProgressButton::ProgressButton(QWidget *parent)
    : QToolButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
}

void ProgressButton::setProgress(double fraction, bool active)
{
    m_fraction = fraction;
    m_active = active;
    update();
}

void ProgressButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    paintCapsule(p, QRectF(rect()), this, isDown());
    const QRectF ring = QRectF(rect()).adjusted(8, 8, -8, -8);
    QColor track = ink(this);
    track.setAlphaF(0.15f);
    p.setPen(QPen(track, 2.4));
    p.drawEllipse(ring);
    if (m_active) {
        p.setPen(QPen(palette().color(QPalette::Highlight), 2.4, Qt::SolidLine, Qt::RoundCap));
        const int span = -int(qBound(0.03, m_fraction, 1.0) * 360 * 16);
        p.drawArc(ring, 90 * 16, span);
    } else {
        const QPixmap icon = Icons::symbol(QStringLiteral("checkmark"), 9, ink(this), devicePixelRatioF(), 1);
        const QSizeF s = icon.deviceIndependentSize();
        p.drawPixmap(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), icon);
    }
}

// ------------------------------------------------------------------ SearchField

SearchField::SearchField(QWidget *parent)
    : QLineEdit(parent)
{
    setFrame(false);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setTextMargins(28, 0, 10, 0);
    setClearButtonEnabled(true);
    setFixedHeight(34);
    setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; }"));
}

void SearchField::paintEvent(QPaintEvent *e)
{
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paintCapsule(p, QRectF(rect()), this);
        const QPixmap icon = Icons::symbol(QStringLiteral("magnifyingglass"), 12, secondaryInk(this), devicePixelRatioF());
        const QSizeF s = icon.deviceIndependentSize();
        p.drawPixmap(QPointF(11, (height() - s.height()) / 2), icon);
    }
    QLineEdit::paintEvent(e);
}

// ------------------------------------------------------------------ DragArea

WindowMover::WindowMover(QWidget *widget, int doubleClickHeight)
    : QObject(widget)
    , m_doubleClickHeight(doubleClickHeight)
{
    widget->installEventFilter(this);
}

void WindowMover::install(QWidget *widget, int doubleClickHeight)
{
    new WindowMover(widget, doubleClickHeight);
}

bool WindowMover::eventFilter(QObject *watched, QEvent *event)
{
    auto *w = static_cast<QWidget *>(watched);
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->button() != Qt::LeftButton)
            break;
        m_dragging = true;
        m_offset = e->globalPosition().toPoint() - w->window()->frameGeometry().topLeft();
        return true;
    }
    case QEvent::MouseMove: {
        auto *e = static_cast<QMouseEvent *>(event);
        if (!m_dragging || !(e->buttons() & Qt::LeftButton))
            break;
        w->window()->move(e->globalPosition().toPoint() - m_offset);
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (m_dragging) {
            m_dragging = false;
            return true;
        }
        break;
    case QEvent::MouseButtonDblClick: {
        auto *e = static_cast<QMouseEvent *>(event);
        if (e->button() == Qt::LeftButton && (m_doubleClickHeight < 0 || e->position().y() < m_doubleClickHeight)) {
            m_dragging = false;
            MacPlatform::titleBarDoubleClick(w);
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

DragArea::DragArea(QWidget *parent)
    : QWidget(parent)
{
    WindowMover::install(this);
}

} // namespace Glass
