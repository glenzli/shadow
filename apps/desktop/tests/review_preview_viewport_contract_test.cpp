#include <QGuiApplication>
#include <QImage>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

class PreviewProvider final : public QQuickImageProvider {
  public:
    PreviewProvider() : QQuickImageProvider(Image) {}
    std::atomic<int> requests{0};
    QImage requestImage(const QString&, QSize* size, const QSize&) override {
        ++requests;
        QImage image(1200, 800, QImage::Format_RGB32);
        image.fill(Qt::darkBlue);
        *size = image.size();
        return image;
    }
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlEngine engine;
    auto* provider = new PreviewProvider;
    engine.addImageProvider(QStringLiteral("filmstrip-test"), provider);
    bool warnings = false;
    QObject::connect(&engine, &QQmlEngine::warnings, [&](const QList<QQmlError>& errors) {
        warnings = true;
        for (const auto& error : errors)
            std::cerr << error.toString().toStdString() << '\n';
    });
    QQmlComponent component(&engine);
    component.setData(
        R"QML(
import QtQuick
import Shadow.ReviewPreviewContract
ReviewPreviewViewport {
    width: 800; height: 500
    source: "image://filmstrip-test/one"
    autoTransform: false
    selectionKey: "photo-one/representation-one"
}
)QML",
        QUrl{}
    );
    std::unique_ptr<QObject> root(component.create());
    if (!root) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QQuickWindow window;
    window.resize(800, 500);
    qobject_cast<QQuickItem*>(root.get())->setParentItem(window.contentItem());
    window.show();
    bool ok = QTest::qWaitFor([&] { return root->property("imageReady").toBool(); }, 3000);
    auto check = [&](bool condition, const char* message) {
        if (!condition)
            std::cerr << message << '\n';
        ok &= condition;
    };
    QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(350, 230));
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint(350, 230));
    QCoreApplication::processEvents();
    check(!root->property("fitView").toBool(), "double-click did not zoom the preview");
    const auto* pan = root->findChild<QObject*>(QStringLiteral("reviewPreviewPan"));
    const double before_pan = pan->property("contentX").toDouble();
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, QPoint(350, 230));
    QTest::mouseMove(&window, QPoint(390, 230), 30);
    QTest::mouseMove(&window, QPoint(440, 230), 30);
    QTest::mouseMove(&window, QPoint(490, 230), 30);
    QTest::qWait(50);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint(490, 230));
    QCoreApplication::processEvents();
    check(
        pan->property("contentX").toDouble() < before_pan,
        "zoomed preview did not pan with the mouse"
    );
    const QPointingDevice device(
        QStringLiteral("trackpad"),
        92,
        QInputDevice::DeviceType::TouchPad,
        QPointingDevice::PointerType::Finger,
        QInputDevice::Capability::Position,
        2,
        0
    );
    auto gesture = [&](Qt::NativeGestureType type, double value) {
        const QPointF point(350, 230);
        QNativeGestureEvent event(type, &device, 2, point, point, point, value, QPointF{});
        QCoreApplication::sendEvent(&window, &event);
        QCoreApplication::processEvents();
    };
    const double before_pinch = root->property("zoomFactor").toDouble();
    gesture(Qt::BeginNativeGesture, 0);
    gesture(Qt::ZoomNativeGesture, 0.25);
    gesture(Qt::EndNativeGesture, 0);
    check(
        std::abs(root->property("zoomFactor").toDouble() - before_pinch * 1.25) < 1e-6,
        "native trackpad pinch did not reach the review preview"
    );
    check(provider->requests == 1, "display-only zoom re-requested image pixels");
    auto* fit =
        qobject_cast<QQuickItem*>(root->findChild<QObject*>(QStringLiteral("reviewPreviewFit")));
    const QPoint fit_point =
        fit->mapToScene(QPointF(fit->width() / 2, fit->height() / 2)).toPoint();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, fit_point);
    check(root->property("fitView").toBool(), "Fit button did not restore the complete image");
    QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(350, 230));
    root->setProperty("selectionKey", QStringLiteral("photo-two/representation-two"));
    check(
        root->property("fitView").toBool() && pan->property("contentX").toDouble() == 0,
        "a newly selected photo inherited the previous zoom or pan"
    );
    return ok && !warnings ? EXIT_SUCCESS : EXIT_FAILURE;
}
