#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QVariant>
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
Item {
    width: 900; height: 600
    Flickable {
        id: pan; objectName: "pan"; anchors.fill: parent
        contentWidth: view.contentWidth; contentHeight: view.contentHeight
    }
    PrecisionViewportState {
        id: view; objectName: "viewport"
        flickable: pan; imagePixelWidth: 6000; imagePixelHeight: 4000; deviceScale: 2
    }
}
)QML", QUrl::fromLocalFile(QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/ViewportContract.qml")));
    std::unique_ptr<QObject> root(component.create());
    if (!root) { std::cerr << component.errorString().toStdString(); return EXIT_FAILURE; }
    auto* view = root->findChild<QObject*>(QStringLiteral("viewport"));
    auto* pan = root->findChild<QObject*>(QStringLiteral("pan"));
    auto invoke = [&](const char* method) { return QMetaObject::invokeMethod(view, method); };
    auto zoom = [&](double x, double y, double value) {
        return QMetaObject::invokeMethod(view, "zoomAt", Q_ARG(QVariant, x),
            Q_ARG(QVariant, y), Q_ARG(QVariant, value));
    };
    auto normalized = [&](const char* method, double value) {
        QVariant result;
        QMetaObject::invokeMethod(view, method, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, value));
        return result.toDouble();
    };
    auto check = [](bool condition, const char* message) {
        if (!condition) std::cerr << message << '\n';
        return condition;
    };
    bool ok = view && pan;
    const double anchor_x = normalized("normalizedX", 420);
    const double anchor_y = normalized("normalizedY", 260);
    invoke("beginContinuousZoom");
    for (double scale : {0.5, 0.75, 1.0, 1.1, 1.8, 3.0, 4.0, 2.0, 0.75}) {
        ok &= zoom(420, 260, scale);
        // Deliberately do not drain queued events between samples. Each input
        // must see a complete transform, even when several arrive in one turn.
        ok &= check(std::abs(normalized("normalizedX", 420) - anchor_x) < 1e-9,
            "horizontal pinch anchor moved between synchronous samples");
        ok &= check(std::abs(normalized("normalizedY", 260) - anchor_y) < 1e-9,
            "vertical pinch anchor moved between synchronous samples");
    }
    invoke("finishContinuousZoom");
    QCoreApplication::processEvents();
    ok &= check(std::abs(normalized("normalizedX", 420) - anchor_x) < 1e-9,
        "deferred work moved the finished gesture");
    // A following gesture starts from the current transform, not the first.
    const double next_anchor = normalized("normalizedX", 600);
    invoke("beginContinuousZoom"); zoom(600, 300, 3.0); invoke("finishContinuousZoom");
    ok &= check(std::abs(normalized("normalizedX", 600) - next_anchor) < 1e-9,
        "second gesture reused an old anchor");
    // Equal centers and pixel scales for smaller comparison panes, including
    // images wider than one pane but narrower than the complete window.
    invoke("reset");
    view->setProperty("viewportWidth", 430.0);
    view->setProperty("viewportHeight", 580.0);
    zoom(450, 300, 0.25);
    ok &= check(std::abs(view->property("imageWidth").toDouble() - 750.0) < 1e-9,
        "comparison changed the physical-pixel zoom scale");
    ok &= check(view->property("contentWidth").toDouble() > 900.0,
        "comparison pane cannot pan to image edges");
    view->setProperty("zoomFactor", 4.0);
    pan->setProperty("contentX", 7000.0);
    invoke("reset");
    ok &= check(pan->property("contentX").toDouble() == 0.0 && view->property("fitView").toBool(),
        "fit reset left a zoomed offset behind");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
