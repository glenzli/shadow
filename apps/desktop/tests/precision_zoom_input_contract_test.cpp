#include <QGuiApplication>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"QML(
import QtQuick
PrecisionCanvasZoomInput {
    width: 900; height: 600
    interactionEnabled: true; toolActive: false; fitView: false
    zoomFactor: 1; fitZoomFactor: 0.2
    property int samples: 0
    onContinuousZoomRequested: (x, y, next) => { zoomFactor = next; samples++ }
}
)QML", QUrl::fromLocalFile(QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/ZoomContract.qml")));
    std::unique_ptr<QObject> root(component.create());
    if (!root) { std::cerr << component.errorString().toStdString(); return EXIT_FAILURE; }
    QQuickWindow window; window.resize(900, 600);
    qobject_cast<QQuickItem*>(root.get())->setParentItem(window.contentItem()); window.show();
    QTest::qWait(30);
    const QPointingDevice device(QStringLiteral("test trackpad"), 123,
        QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
        QInputDevice::Capability::Position, 2, 0);
    const auto gesture = [&](Qt::NativeGestureType type, double value) {
        const QPointF point(450, 300);
        QNativeGestureEvent event(type, &device, 2, point, point, point, value, QPointF{});
        QCoreApplication::sendEvent(&window, &event);
        QCoreApplication::processEvents();
    };
    for (int sequence = 0; sequence < 3; ++sequence) {
        gesture(Qt::BeginNativeGesture, 0);
        gesture(Qt::ZoomNativeGesture, 0.25);
        gesture(Qt::EndNativeGesture, 0);
        const double expected = std::pow(1.25, sequence + 1);
        if (std::abs(root->property("zoomFactor").toDouble() - expected) > 1e-6) {
            std::cerr << "native pinch accumulated an earlier gesture: expected " << expected
                << " got " << root->property("zoomFactor").toDouble() << '\n';
            return EXIT_FAILURE;
        }
    }
    return root->property("samples").toInt() >= 3 ? EXIT_SUCCESS : EXIT_FAILURE;
}
