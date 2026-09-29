#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Variant selector contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    QQmlComponent editor_component(&engine);
    editor_component.setData(
        R"QML(
        import QtQml
        QtObject {
            property bool active: true
            property bool variantActionsEnabled: true
            property var photoVariants: []
        }
    )QML",
        QUrl{}
    );
    std::unique_ptr<QObject> editor(editor_component.create());
    if (!editor) {
        std::cerr << editor_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    auto set_variants = [&](const QString& name) {
        editor->setProperty(
            "photoVariants",
            QVariantList{QVariantMap{
                {QStringLiteral("name"), name},
                {QStringLiteral("isActive"), true},
                {QStringLiteral("isDefault"), true},
                {QStringLiteral("variantId"), QStringLiteral("original")},
            }}
        );
        QCoreApplication::processEvents();
    };
    set_variants(QString{});
    QQmlComponent selector_component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionVariantSelector.qml")
        )
    );
    std::unique_ptr<QObject> selector(selector_component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(editor.get())},
    }));
    if (!selector) {
        std::cerr << selector_component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QObject* const button =
        selector->findChild<QObject*>(QStringLiteral("precisionVariantSelectorButton"));
    if (!require(
            button && button->property("text") == QStringLiteral("Original"),
            "the catalog's unnamed original has a visible toolbar label"
        )) {
        return EXIT_FAILURE;
    }
    QQuickWindow window;
    window.resize(360, 240);
    qobject_cast<QQuickItem*>(selector.get())->setParentItem(window.contentItem());
    window.show();
    QMetaObject::invokeMethod(button, "clicked");
    QCoreApplication::processEvents();
    QObject* const menu = selector->findChild<QObject*>(QStringLiteral("precisionVariantMenu"));
    if (!require(
            menu && menu->property("visible").toBool(),
            "the variants menu opens from its toolbar button"
        )) {
        return EXIT_FAILURE;
    }
    QTest::keyClick(&window, Qt::Key_Escape);
    QTest::qWait(200);
    if (!require(
            !menu->property("visible").toBool(),
            "Escape dismisses the variants menu without a pointer click"
        )) {
        return EXIT_FAILURE;
    }
    auto check_menu = [&](const QString& name, bool is_default, const QString& expected) {
        QVariant result;
        const QVariant variant = QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("isDefault"), is_default},
        };
        return QMetaObject::invokeMethod(
                   selector.get(),
                   "menuName",
                   Q_RETURN_ARG(QVariant, result),
                   Q_ARG(QVariant, variant)
               )
               && result.toString() == expected;
    };
    if (!require(
            check_menu(QString{}, true, QStringLiteral("Original")),
            "the unnamed original menu label has no dangling separator or duplicate"
        )
        || !require(
            check_menu(QStringLiteral("Studio"), true, QStringLiteral("Studio · Original")),
            "a renamed original retains its name and original marker"
        )
        || !require(
            check_menu(QStringLiteral("Warm"), false, QStringLiteral("Warm")),
            "named authored variants retain their labels"
        )) {
        return EXIT_FAILURE;
    }
    set_variants(QStringLiteral("Studio"));
    if (!require(
            button->property("text") == QStringLiteral("Studio"),
            "the toolbar reacts to a renamed active variant"
        )) {
        return EXIT_FAILURE;
    }
    set_variants(QStringLiteral("   "));
    return require(
               button->property("text") == QStringLiteral("Original"),
               "legacy whitespace names also receive a visible fallback"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
