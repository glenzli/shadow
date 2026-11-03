#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTranslator>
#include <memory>

QQuickItem* visualItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems()) {
        if (auto* found = visualItem(child, name))
            return found;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTranslator chinese;
    if (!chinese.load(QStringLiteral(SHADOW_TRANSFER_TRANSLATION)))
        return 1;
    for (const bool translated : {false, true}) {
        if (translated)
            app.installTranslator(&chinese);
        QQmlEngine engine;
        bool warnings = false;
        QObject::connect(&engine, &QQmlEngine::warnings, [&](const auto&) { warnings = true; });
        QQmlComponent component(&engine);
        component.setData(
            R"QML(
import QtQuick
import Shadow.AdjustmentTransferContract
Item {
    id: root
    width: 700; height: 650
    property QtObject transfer: QtObject {
        property bool admissionEnabled: true
        property bool busy: false
        property bool copying: false
        property bool preparingCopy: false
        property string copySourceName: "Photo A"
        property string copyError: ""
        property var copyOptions: [{nodeId: "look", label: "Look", items: [
            {key: "exposure", label: "Exposure"}, {key: "saturation", label: "Saturation"}]}]
        property var copied: []
        property var sourceTarget: null
        property bool spatial: false
        property var results: []
        property string batchSourceName: ""
        property string statusText: ""
        property int completed: 0
        property int failed: 0
        property bool canRetry: false
        property bool canUndo: false
        function prepareCopy(target) { sourceTarget = target; return true }
        function copyChosenAdjustments(choices, allowSpatial) { copied = choices; spatial = allowSpatial }
    }
    AdjustmentTransferDialog { id: dialog; transferController: root.transfer }
    function present() { dialog.presentCopy({photoId: "source", sourcePath: "/isolated/source.dng"}) }
}
)QML",
            QUrl{}
        );
        QQuickWindow window;
        window.resize(700, 650);
        std::unique_ptr<QObject> root(component.create());
        if (!root)
            qFatal("%s", qPrintable(component.errorString()));
        auto* root_item = qobject_cast<QQuickItem*>(root.get());
        root_item->setParentItem(window.contentItem());
        window.show();
        QMetaObject::invokeMethod(root.get(), "present");
        auto* dialog = root->findChild<QObject*>(QStringLiteral("adjustmentTransferDialog"));
        if (!dialog || !QTest::qWaitFor([&] { return dialog->property("visible").toBool(); }))
            return 2;
        auto* masks =
            visualItem(window.contentItem(), QStringLiteral("copyRelativePositionMasksOption"));
        if (!masks || masks->isVisible())
            return 5;
        const auto click = [&](const char* name) {
            auto* item = visualItem(window.contentItem(), QString::fromLatin1(name));
            if (!item || !item->isVisible() || !item->isEnabled())
                qFatal("Unreachable transfer action: %s", name);
            QTest::mouseClick(
                &window,
                Qt::LeftButton,
                Qt::NoModifier,
                item->mapToScene(item->boundingRect().center()).toPoint()
            );
            QCoreApplication::processEvents();
        };
        click("expandCopyNode_look");
        click("copyItem_look_exposure");
        auto* transfer = root->property("transfer").value<QObject*>();
        click("confirmCopyAdjustmentsButton");
        const auto copied = transfer->property("copied").value<QJSValue>().toVariant().toList();
        if (copied.size() != 1
            || copied.first().toMap().value("nodeId").toString() != QStringLiteral("look"))
            return 3;
        const auto keys = copied.first().toMap().value("keys").toStringList();
        if (keys != QStringList{QStringLiteral("saturation")} || warnings
            || transfer->property("spatial").toBool())
            return 4;
    }
    return 0;
}
