#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTest>
#include <QVariant>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
bool require(bool condition, const char* message) {
    if (!condition)
        std::cerr << "Export workflow: " << message << '\n';
    return condition;
}
} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"QML(
        import QtQuick
        import QtQuick.Controls
        import "."
        ApplicationWindow {
            width: 1000; height: 800; visible: true
            QtObject {
                id: exporter
                objectName: "exporter"
                property bool busy: false
                property bool cancellationRequested: false
                property int totalCount: 0
                property int currentCount: 0
                property string statusText: "Ready"
                property var errors: []
                property var presets: []
                property var watermarks: []
                property int cancelCalls: 0
                function cancelExport() { ++cancelCalls; cancellationRequested = true }
                signal exportFinished(int completed, int failed, bool cancelled, var paths, var errors)
            }
            ExportDialog {
                id: dialog
                objectName: "dialog"
                exportController: exporter
            }
            ExportActivityButton {
                exportController: exporter
                onClicked: dialog.open()
            }
        }
    )QML", QUrl::fromLocalFile(QStringLiteral(
        SHADOW_DESKTOP_SOURCE_DIR "/qml/export-workflow-harness.qml")));
    std::unique_ptr<QObject> root(component.create());
    if (!root) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    auto* exporter = root->findChild<QObject*>(QStringLiteral("exporter"));
    auto* dialog = root->findChild<QObject*>(QStringLiteral("dialog"));
    auto* activity = root->findChild<QObject*>(QStringLiteral("exportActivityButton"));
    auto* background = root->findChild<QObject*>(QStringLiteral("exportBackgroundButton"));
    auto* cancel = root->findChild<QObject*>(QStringLiteral("exportCancelButton"));
    if (!require(window && exporter && dialog && activity && background && cancel,
                 "production surfaces load"))
        return EXIT_FAILURE;
    const QVariant original = QVariantList{QVariantMap{{"photoId", "original"}}};
    QMetaObject::invokeMethod(dialog, "present", Q_ARG(QVariant, original));
    exporter->setProperty("busy", true);
    exporter->setProperty("totalCount", 1);
    QTest::qWait(200);
    if (!require(dialog->property("visible").toBool()
                     && activity->property("visible").toBool(),
                 "running export has a global progress entry"))
        return EXIT_FAILURE;
    QMetaObject::invokeMethod(background, "clicked");
    QTest::qWait(200);
    if (!require(!dialog->property("visible").toBool()
                     && exporter->property("cancelCalls").toInt() == 0,
                 "continue editing hides the dialog without cancelling"))
        return EXIT_FAILURE;
    const QVariant changed = QVariantList{QVariantMap{{"photoId", "other"}}};
    QMetaObject::invokeMethod(dialog, "present", Q_ARG(QVariant, changed));
    QTest::qWait(200);
    if (!require(dialog->property("targets") == original,
                 "a new selection cannot replace the running export targets"))
        return EXIT_FAILURE;
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(200);
    if (!require(!dialog->property("visible").toBool()
                     && exporter->property("cancelCalls").toInt() == 0,
                 "Escape returns to editing without cancelling"))
        return EXIT_FAILURE;
    QMetaObject::invokeMethod(activity, "clicked");
    QTest::qWait(200);
    if (!require(dialog->property("visible").toBool(),
                 "global progress reopens the running job"))
        return EXIT_FAILURE;
    QMetaObject::invokeMethod(cancel, "clicked");
    if (!require(exporter->property("cancelCalls").toInt() == 1,
                 "only the explicit cancel action requests cancellation"))
        return EXIT_FAILURE;
    exporter->setProperty("busy", false);
    exporter->setProperty("cancellationRequested", false);
    exporter->setProperty("currentCount", 1);
    exporter->setProperty("statusText", QStringLiteral("Exported 1 photo"));
    QMetaObject::invokeMethod(exporter, "exportFinished", Q_ARG(int, 1), Q_ARG(int, 0),
        Q_ARG(bool, false), Q_ARG(QVariant, QVariantList{}), Q_ARG(QVariant, QVariantList{}));
    QTest::qWait(200);
    if (!require(dialog->property("visible").toBool()
                     && activity->property("text") == QStringLiteral("Exported 1 photo"),
                 "completion keeps its receipt visible and globally reachable"))
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
