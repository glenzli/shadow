#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QVariantMap>
#include <cstdlib>
#include <iostream>
#include <memory>

class DelayedDetailProvider final : public QQuickImageProvider {
public:
    DelayedDetailProvider() : QQuickImageProvider(Image, ForceAsynchronousImageLoading) {}
    QImage requestImage(const QString& id, QSize* size, const QSize&) override {
        if (id.startsWith(QStringLiteral("slow"))) QThread::msleep(120);
        QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::red);
        *size = image.size(); return image;
    }
};

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("detail-test"), new DelayedDetailProvider);
    QQmlComponent component(&engine, QUrl::fromLocalFile(
        QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionDetailSurface.qml")));
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {QStringLiteral("tile"), QVariantMap{}}, {QStringLiteral("displayScale"), 2.0}}));
    if (!root) { std::cerr << component.errorString().toStdString(); return EXIT_FAILURE; }
    QQuickWindow window;
    auto* item = qobject_cast<QQuickItem*>(root.get()); item->setParentItem(window.contentItem());
    window.resize(400, 300); window.show();
    const auto tile = [](const char* source, int x) {
        return QVariantMap{{QStringLiteral("source"), QString::fromLatin1(source)},
            {QStringLiteral("x"), x}, {QStringLiteral("y"), 20},
            {QStringLiteral("width"), 100}, {QStringLiteral("height"), 80}};
    };
    const auto wait = [](auto predicate) {
        QElapsedTimer timer; timer.start();
        while (!predicate() && timer.elapsed() < 2000) {
            QCoreApplication::processEvents(); QThread::msleep(1);
        }
        return predicate();
    };
    const auto active = [&]() {
        return root->findChild<QQuickItem*>(root->property("activeSlot").toInt() == 0
            ? QStringLiteral("firstDetailSlot") : QStringLiteral("secondDetailSlot"));
    };
    bool ok = true;
    root->setProperty("tile", tile("image://detail-test/first", 10));
    ok &= wait([&] { return root->property("ready").toBool(); });
    ok &= active()->x() == 20.0;
    root->setProperty("tile", tile("image://detail-test/slow-second", 200));
    QCoreApplication::processEvents();
    ok &= root->property("ready").toBool() && active()->x() == 20.0;
    ok &= wait([&] { return root->property("pendingSlot").toInt() == -1; });
    ok &= active()->x() == 400.0;
    // The pixels and rectangle advance together; a superseded async load must
    // never replace a newer viewport or reuse its placement for old pixels.
    root->setProperty("tile", tile("image://detail-test/slow-third", 300));
    root->setProperty("tile", tile("image://detail-test/latest", 400));
    ok &= wait([&] { return root->property("pendingSlot").toInt() == -1; });
    ok &= active()->x() == 800.0;
    root->setProperty("tile", QVariantMap{});
    ok &= !root->property("ready").toBool();
    if (!ok) std::cerr << "detail pixels and placement did not remain one atomic presentation\n";
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
