#include "paint_stroke_input.hpp"
#include <QCursor>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QQuickWindow>
#include <QTabletEvent>
#include <algorithm>

PaintStrokeInput::PaintStrokeInput(QQuickItem* parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setCursor(QCursor(Qt::BlankCursor));
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* window) {
        cancel();
        if (observed_window_)
            observed_window_->removeEventFilter(this);
        observed_window_ = window;
        if (window)
            window->installEventFilter(this);
    });
    observed_window_ = window();
    if (observed_window_)
        observed_window_->installEventFilter(this);
    connect(this, &QQuickItem::enabledChanged, this, [this] {
        if (!isEnabled())
            cancel();
    });
    connect(this, &QQuickItem::visibleChanged, this, [this] {
        if (!isVisible())
            cancel();
    });
}
PaintStrokeInput::~PaintStrokeInput() {
    // QQuickItem's base destructor can emit windowChanged after our members die.
    disconnect(this, nullptr, this, nullptr);
    if (observed_window_)
        observed_window_->removeEventFilter(this);
}
void PaintStrokeInput::setPointerCursor(int value) {
    cursor_ = value;
    setCursor(QCursor(static_cast<Qt::CursorShape>(value)));
}
void PaintStrokeInput::updatePointer(QPointF position, double pressure) {
    position_ = position;
    pressure_ = std::clamp(pressure, 0.0, 1.0);
    hover_ = contains(position);
    emit pointerChanged();
}
void PaintStrokeInput::cancel() {
    const bool was_pressed = pressed_;
    pressed_ = false;
    tablet_ = nullptr;
    hover_ = false;
    setKeepMouseGrab(false);
    if (was_pressed)
        emit strokeCanceled();
    emit pointerChanged();
}
bool PaintStrokeInput::eventFilter(QObject* watched, QEvent* event) {
    if (watched != observed_window_)
        return false;
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide) {
        cancel();
        return false;
    }
    const auto type = event->type();
    if (type != QEvent::TabletPress && type != QEvent::TabletMove && type != QEvent::TabletRelease)
        return false;
    if (!isEnabled() || !isVisible())
        return false;
    auto* e = static_cast<QTabletEvent*>(event);
    const auto point = mapFromScene(e->position());
    if (tablet_ && tablet_ != e->pointingDevice())
        return false;
    if (type == QEvent::TabletPress) {
        if (pressed_ || !contains(point) || !(e->buttons() & Qt::LeftButton))
            return false;
        pressed_ = true;
        tablet_ = e->pointingDevice();
        updatePointer(point, e->pressure());
        emit strokePressed(
            point.x(),
            point.y(),
            pressure_,
            int(e->modifiers()),
            tablet_->pointerType() == QPointingDevice::PointerType::Eraser
        );
    } else if (!tablet_) {
        if (!contains(point)) {
            if (hover_) {
                hover_ = false;
                emit pointerChanged();
            }
            return false;
        }
        updatePointer(point, 1);
        return false;
    } else if (type == QEvent::TabletRelease) {
        // Release pressure is normally zero. Keep the final contact sample rather
        // than retroactively making a stationary terminal dab disappear.
        updatePointer(point, pressure_);
        emit strokeMoved(point.x(), point.y(), pressure_);
        pressed_ = false;
        tablet_ = nullptr;
        emit strokeReleased();
        emit pointerChanged();
    } else {
        updatePointer(point, e->pressure());
        emit strokeMoved(point.x(), point.y(), pressure_);
    }
    e->accept();
    return true;
}
void PaintStrokeInput::mousePressEvent(QMouseEvent* e) {
    if (pressed_ || e->source() != Qt::MouseEventNotSynthesized) {
        e->accept();
        return;
    }
    pressed_ = true;
    setKeepMouseGrab(true);
    updatePointer(e->position(), 1);
    emit strokePressed(position_.x(), position_.y(), 1, int(e->modifiers()), false);
    e->accept();
}
void PaintStrokeInput::mouseMoveEvent(QMouseEvent* e) {
    if (tablet_ || e->source() != Qt::MouseEventNotSynthesized) {
        e->accept();
        return;
    }
    updatePointer(e->position(), 1);
    if (pressed_)
        emit strokeMoved(position_.x(), position_.y(), 1);
    e->accept();
}
void PaintStrokeInput::mouseReleaseEvent(QMouseEvent* e) {
    if (!pressed_ || tablet_ || e->source() != Qt::MouseEventNotSynthesized) {
        e->accept();
        return;
    }
    updatePointer(e->position(), 1);
    emit strokeMoved(position_.x(), position_.y(), 1);
    pressed_ = false;
    setKeepMouseGrab(false);
    emit strokeReleased();
    emit pointerChanged();
    e->accept();
}
void PaintStrokeInput::mouseUngrabEvent() {
    if (pressed_ && !tablet_)
        cancel();
}
void PaintStrokeInput::hoverMoveEvent(QHoverEvent* e) {
    if (!tablet_)
        updatePointer(e->position(), 1);
}
void PaintStrokeInput::hoverLeaveEvent(QHoverEvent*) {
    hover_ = false;
    emit pointerChanged();
}
