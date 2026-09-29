#pragma once
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>
class QQuickWindow;
class QPointingDevice;

// One captured mouse/tablet gesture. Tablet events are consumed before mouse
// synthesis, retaining real pressure and the pen's eraser end.
class PaintStrokeInput : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QPointF pointerPosition READ pointerPosition NOTIFY pointerChanged)
    Q_PROPERTY(bool containsPointer READ containsPointer NOTIFY pointerChanged)
    Q_PROPERTY(double pressure READ pressure NOTIFY pointerChanged)
    Q_PROPERTY(int pointerCursor READ pointerCursor WRITE setPointerCursor)
  public:
    explicit PaintStrokeInput(QQuickItem* parent = nullptr);
    ~PaintStrokeInput() override;
    QPointF pointerPosition() const {
        return position_;
    }
    bool containsPointer() const {
        return hover_ || pressed_;
    }
    double pressure() const {
        return pressure_;
    }
    int pointerCursor() const {
        return cursor_;
    }
    void setPointerCursor(int value);
    Q_INVOKABLE void cancel();
  signals:
    void pointerChanged();
    void strokePressed(double x, double y, double pressure, int modifiers, bool eraser);
    void strokeMoved(double x, double y, double pressure);
    void strokeReleased();
    void strokeCanceled();

  protected:
    bool eventFilter(QObject*, QEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent*) override;
    void hoverLeaveEvent(QHoverEvent*) override;

  private:
    void updatePointer(QPointF position, double pressure);
    QPointer<QQuickWindow> observed_window_;
    const QPointingDevice* tablet_ = nullptr;
    QPointF position_;
    double pressure_ = 1;
    bool hover_ = false, pressed_ = false;
    int cursor_ = Qt::BlankCursor;
};
