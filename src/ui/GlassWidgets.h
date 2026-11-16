#pragma once

#include <QLineEdit>
#include <QToolButton>
#include <QWidget>

// Small set of widgets that give the toolbar the macOS 26 "Liquid Glass"
// look: floating capsules with a translucent fill and hairline border.
namespace Glass {

bool isDark(const QWidget *w);
QColor fill(const QWidget *w, bool pressed = false);
QColor border(const QWidget *w);
QColor ink(const QWidget *w, bool enabled = true);
QColor secondaryInk(const QWidget *w);
void paintCapsule(QPainter &p, const QRectF &r, const QWidget *w, bool pressed = false);

// Capsule container that lays out its children horizontally.
class Group : public QWidget
{
    Q_OBJECT
public:
    explicit Group(QWidget *parent = nullptr, int padding = 0);
protected:
    void paintEvent(QPaintEvent *) override;
};

// Icon button drawn with an SF Symbol; optional text and menu arrow.
class Button : public QToolButton
{
    Q_OBJECT
public:
    explicit Button(const QString &symbol, const QString &accessibleName, QWidget *parent = nullptr);
    void setSymbol(const QString &symbol);
    void setStandalone(bool standalone) { m_standalone = standalone; update(); }  // own capsule background
    void setShowMenuArrow(bool on) { m_menuArrow = on; updateGeometry(); update(); }
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    QString m_symbol;
    bool m_standalone = false;
    bool m_menuArrow = false;
    bool m_hover = false;
};

// Circular progress indicator button for transfers.
class ProgressButton : public QToolButton
{
    Q_OBJECT
public:
    explicit ProgressButton(QWidget *parent = nullptr);
    void setProgress(double fraction, bool active);
    QSize sizeHint() const override { return {34, 34}; }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    double m_fraction = 0;
    bool m_active = false;
};

class SearchField : public QLineEdit
{
    Q_OBJECT
public:
    explicit SearchField(QWidget *parent = nullptr);
    QSize sizeHint() const override { return {200, 34}; }
protected:
    void paintEvent(QPaintEvent *) override;
};

// Makes the empty space of a widget behave like a title bar: drag to move
// the window, double-click to zoom/minimise (system preference). Clicks that
// land on child controls are not affected. `doubleClickHeight` limits the
// double-click zone to the top N pixels (-1 = whole widget).
class WindowMover : public QObject
{
    Q_OBJECT
public:
    static void install(QWidget *widget, int doubleClickHeight = -1);
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    WindowMover(QWidget *widget, int doubleClickHeight);
    int m_doubleClickHeight;
    bool m_dragging = false;
    QPoint m_offset;
};

// Empty toolbar space that drags the window (title bar behaviour).
class DragArea : public QWidget
{
    Q_OBJECT
public:
    explicit DragArea(QWidget *parent = nullptr);
};

} // namespace Glass
