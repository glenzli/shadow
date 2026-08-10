#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "review gallery section navigator failed: " << message << '\n';
    }
    return condition;
}

void drainBindings(const int delay_ms = 0) {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    if (delay_ms > 0) {
        QTest::qWait(delay_ms);
        QCoreApplication::processEvents();
    }
}

[[nodiscard]] QVariantList sectionAnchors() {
    QVariantList anchors;
    for (int index = 0; index < 8; ++index) {
        anchors.push_back(
            QVariantMap{
                {QStringLiteral("key"), QStringLiteral("week-%1").arg(index + 1)},
                {QStringLiteral("title"),
                 QStringLiteral("2026 · Week %1 · Beijing").arg(index + 1)},
                {QStringLiteral("navigationLabel"),
                 QStringLiteral("2026 · Week %1").arg(index + 1)},
                {QStringLiteral("navigationShortLabel"), QStringLiteral("W%1").arg(index + 1)},
                {QStringLiteral("navigationMajorLabel"), QStringLiteral("2026")},
                {QStringLiteral("rowIndex"), index * 5},
                {QStringLiteral("ordinal"), index},
            }
        );
    }
    return anchors;
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    QQmlEngine engine;

    QQmlComponent list_component{&engine};
    list_component.setData(
        R"qml(
            import QtQuick
            ListView {
                width: 740
                height: 480
                model: 40
                delegate: Item {
                    required property int index
                    width: ListView.view.width
                    height: 80
                }
            }
        )qml",
        QUrl{QStringLiteral("inmemory:/SectionNavigatorList.qml")}
    );
    while (list_component.status() == QQmlComponent::Loading) {
        QCoreApplication::processEvents();
    }
    if (list_component.status() == QQmlComponent::Error) {
        std::cerr << list_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    std::unique_ptr<QObject> list_object{list_component.create()};
    auto* const list = qobject_cast<QQuickItem*>(list_object.get());
    if (list == nullptr) {
        std::cerr << list_component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QQmlComponent navigator_component{&engine};
    navigator_component.loadFromModule(
        QStringLiteral("Shadow.ReviewGallerySectionNavigatorContract"),
        QStringLiteral("ReviewGallerySectionNavigator")
    );
    while (navigator_component.status() == QQmlComponent::Loading) {
        QCoreApplication::processEvents();
    }
    if (navigator_component.status() == QQmlComponent::Error) {
        std::cerr << navigator_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    std::unique_ptr<QObject> navigator_object{navigator_component.createWithInitialProperties({
        {QStringLiteral("view"), QVariant::fromValue(list)},
        {QStringLiteral("sectionAnchors"), sectionAnchors()},
        {QStringLiteral("groupingActive"), true},
    })};
    auto* const navigator = qobject_cast<QQuickItem*>(navigator_object.get());
    if (navigator == nullptr) {
        std::cerr << navigator_component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    QQuickWindow window;
    window.setGeometry(0, 0, 800, 540);
    list->setParentItem(window.contentItem());
    list->setPosition(QPointF{20.0, 20.0});
    navigator->setParentItem(window.contentItem());
    navigator->setPosition(QPointF{708.0, 80.0});
    window.show();
    drainBindings(80);

    bool valid = true;
    valid &= require(
        navigator->property("eligible").toBool(),
        "a grouped scrollable gallery admits the transient navigator"
    );
    valid &= require(
        navigator->opacity() < 0.05,
        "the navigator starts hidden instead of becoming permanent chrome"
    );

    list->setProperty("contentY", 240.0);
    drainBindings(240);
    valid &=
        require(navigator->opacity() > 0.9, "a real ListView scroll reveals the section navigator");

    auto* const input =
        navigator->findChild<QQuickItem*>(QStringLiteral("reviewGallerySectionNavigatorInput"));
    auto* const preview =
        navigator->findChild<QQuickItem*>(QStringLiteral("reviewGallerySectionNavigatorPreview"));
    valid &=
        require(input != nullptr && preview != nullptr, "pointer and preview owners are packaged");
    if (input != nullptr && preview != nullptr) {
        const QPointF press_position =
            navigator->mapToScene(QPointF{navigator->width() - 5.0, navigator->height() * 0.32});
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, press_position.toPoint());
        drainBindings(80);
        valid &= require(
            navigator->property("dragging").toBool() && preview->isVisible(),
            "pressing the rail begins one previewing scrub lifecycle"
        );

        const QPointF release_position =
            navigator->mapToScene(QPointF{navigator->width() - 5.0, navigator->height() - 15.0});
        QTest::mouseMove(&window, release_position.toPoint(), 80);
        drainBindings(80);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, release_position.toPoint());
        drainBindings(160);
        valid &= require(
            list->property("contentY").toDouble() > 1'800.0,
            "dragging near the rail end jumps to a later concrete section row"
        );
        valid &= require(
            !navigator->property("dragging").toBool() && !preview->isVisible(),
            "release closes the preview and completes the admitted gesture"
        );
    }

    QTest::mouseMove(&window, QPoint{80, 80}, 20);
    drainBindings(2'100);
    valid &= require(
        navigator->opacity() < 0.05,
        "the navigator fades after scrolling and pointer interaction stop"
    );

    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
