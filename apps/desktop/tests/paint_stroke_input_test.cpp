#include "paint_stroke_input.hpp"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QQuickWindow>
#include <QTabletEvent>
#include <cmath>
#include <cstdlib>
#include <iostream>
namespace {
void check(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickWindow window;
    window.resize(320, 240);
    PaintStrokeInput input(window.contentItem());
    input.setPosition({20, 30});
    input.setSize({200, 150});
    window.show();
    QCoreApplication::processEvents();
    int pressed = 0, moved = 0, released = 0, canceled = 0;
    double pressure = 0, x = 0;
    bool eraser = false;
    QObject::connect(
        &input,
        &PaintStrokeInput::strokePressed,
        [&](double px, double, double p, int, bool e) {
            ++pressed;
            x = px;
            pressure = p;
            eraser = e;
        }
    );
    QObject::connect(&input, &PaintStrokeInput::strokeMoved, [&](double, double, double p) {
        ++moved;
        pressure = p;
    });
    QObject::connect(&input, &PaintStrokeInput::strokeReleased, [&] { ++released; });
    QObject::connect(&input, &PaintStrokeInput::strokeCanceled, [&] { ++canceled; });
    const auto mouse = [&](QEvent::Type type,
                           QPointF pos,
                           Qt::MouseButton button,
                           Qt::MouseButtons buttons,
                           Qt::MouseEventSource source = Qt::MouseEventNotSynthesized) {
        QMouseEvent e(
            type,
            pos,
            pos,
            window.mapToGlobal(pos.toPoint()),
            button,
            buttons,
            Qt::NoModifier,
            source
        );
        QCoreApplication::sendEvent(&window, &e);
    };
    mouse(QEvent::MouseButtonPress, {40, 50}, Qt::LeftButton, Qt::LeftButton);
    check(pressed == 1 && x == 20 && pressure == 1, "real mouse maps into item at full pressure");
    mouse(QEvent::MouseMove, {300, 230}, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, {300, 230}, Qt::LeftButton, Qt::NoButton);
    check(moved >= 1 && released == 1, "mouse capture releases outside canvas");
    QPointingDevice pen(
        "test pen",
        21,
        QInputDevice::DeviceType::Stylus,
        QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure,
        1,
        1
    );
    QPointingDevice rubber(
        "test eraser",
        22,
        QInputDevice::DeviceType::Stylus,
        QPointingDevice::PointerType::Eraser,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure,
        1,
        1
    );
    const auto tablet =
        [&](QEvent::Type type, const QPointingDevice* device, QPointF pos, double p) {
            const auto buttons = type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton;
            QTabletEvent e(
                type,
                device,
                pos,
                window.mapToGlobal(pos.toPoint()),
                p,
                0,
                0,
                0,
                0,
                0,
                Qt::NoModifier,
                type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                buttons
            );
            QCoreApplication::sendEvent(&window, &e);
            return e.isAccepted();
        };
    check(
        tablet(QEvent::TabletPress, &pen, {60, 60}, 0.23),
        "tablet event consumed before mouse synthesis"
    );
    check(
        pressed == 2 && x == 40 && std::abs(pressure - 0.23) < 1e-5 && !eraser,
        "pen pressure retained"
    );
    mouse(
        QEvent::MouseButtonPress,
        {60, 60},
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::MouseEventSynthesizedByQt
    );
    check(pressed == 2, "synthesized mouse does not duplicate tablet stroke");
    tablet(QEvent::TabletMove, &pen, {310, 230}, 0.78);
    check(std::abs(pressure - 0.78) < 1e-5, "pressure follows captured motion outside canvas");
    tablet(QEvent::TabletRelease, &pen, {310, 230}, 0);
    check(
        released == 2 && std::abs(pressure - 0.78) < 1e-5,
        "release retains last contact pressure"
    );
    tablet(QEvent::TabletPress, &rubber, {60, 60}, 0.4);
    check(pressed == 3 && eraser, "physical eraser distinguished");
    input.setEnabled(false);
    check(canceled == 1, "disabling input cancels captured gesture");
    tablet(QEvent::TabletMove, &rubber, {60, 60}, 0.8);
    check(released == 2 && canceled == 1, "disabled input cannot complete canceled gesture");
    input.setEnabled(true);
    tablet(QEvent::TabletPress, &pen, {60, 60}, 0.4);
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(&window, &deactivate);
    check(canceled == 2, "window deactivation cancels tablet gesture");
    std::cout << "Paint input: mouse/tablet capture, pressure, eraser and cancellation passed\n";
}
