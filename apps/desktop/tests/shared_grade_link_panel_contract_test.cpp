#include <QDir>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTranslator>

#include <cstdlib>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        qCritical() << message;
        std::exit(1);
    }
}
void click(QQuickWindow& window, QObject* root, const char* name) {
    auto* item = root->findChild<QQuickItem*>(QString::fromLatin1(name));
    require(item && item->isVisible() && item->isEnabled(), name);
    QTest::mouseClick(
        &window,
        Qt::LeftButton,
        Qt::NoModifier,
        item->mapToScene(item->boundingRect().center()).toPoint()
    );
    QTest::qWait(40);
}
void check_bounds(QQuickItem* item, QQuickItem* content) {
    if (!item->isVisible())
        return;
    const auto bounds = item->mapRectToItem(content, item->boundingRect());
    require(
        bounds.width() == 0 || (bounds.left() >= -3 && bounds.right() <= content->width() + 3),
        "shared link content fits horizontally"
    );
    for (auto* child : item->childItems())
        check_bounds(child, content);
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTranslator chinese;
    bool has_chinese = false;
#ifdef SHADOW_SHARED_GRADE_LINK_TRANSLATION
    has_chinese = chinese.load(QStringLiteral(SHADOW_SHARED_GRADE_LINK_TRANSLATION));
    require(has_chinese, "finished Chinese translation loads");
#endif
    for (bool translated : {false, true}) {
        if (translated && !has_chinese)
            continue;
        if (translated)
            app.installTranslator(&chinese);
        for (bool dark : {false, true}) {
            QQmlEngine engine;
            QQmlComponent fake(&engine);
            fake.setData(
                R"(
import QtQml
QtObject {
 property bool busy: false
 property bool admissionEnabled: true
 property bool canRetry: false
 property string sourceName: "Source photo"
 property string requestSourceName: "Source photo"
 property string requestSourcePhotoId: "source"
 property string nodeName: "Shared tone"
 property string statusText: ""
 property int total: 0
 property int completed: 0
 property int failed: 0
 property var results: []
 property int publishCount: 0
 property int linkCount: 0
 property int retryCount: 0
 property int cancelCount: 0
 property int publicationRequestCount: 0
 property int linkRequestCount: 0
 property int syncCount: 0
 property bool usedSpatialMasks: false
 property var appliedTargets: []
 signal presentationRequested(string mode)
 function requestPublication() { ++publicationRequestCount; presentationRequested("publish") }
 function requestLinks() { ++linkRequestCount; presentationRequested("link") }
 function syncSelected() { ++syncCount }
 function publish(label, targets, spatial) {
  ++publishCount; appliedTargets = targets; usedSpatialMasks = spatial; busy = true
  total = 2; completed = 1; statusText = "Updating shared links · 1 completed"
  results = [ {photoId: "source", title: "Source photo", state: "published", message: "Published and linked", detail: ""} ]
 }
 function linkToPhotos(targets, spatial) { ++linkCount; appliedTargets = targets; usedSpatialMasks = spatial; busy = true }
 function retryFailed() { ++retryCount }
 function cancel() { ++cancelCount; busy = false; canRetry = true }
})",
                QUrl()
            );
            std::unique_ptr<QObject> controller(fake.create());
            require(bool(controller), "controller projection creates");
            QQmlComponent fake_editor(&engine);
            fake_editor.setData(
                R"(
import QtQml
QtObject {
 property bool active: true
 property bool stateBusy: false
 property bool hasSelectedGradeNode: true
 property bool canAddGradeNode: true
 property bool canDeleteGradeNode: true
 property string selectedRecipeNodeKind: "grade"
 property var sharedGradeNodes: []
 property var selectedSharedGradeNodeState: ({sharedFollowHead: true, sharedLocallyModified: false, sharedUpdateAvailable: false})
 property int unlinkCount: 0
 property int followCount: 0
 property bool followed: true
 function unlinkSelectedGradeNode() { ++unlinkCount }
 function setSelectedGradeNodeSharedFollowHead(value) { ++followCount; followed = value }
 function refreshSharedGradeNodes() {}
 function duplicateSelectedGradeNode() {}
 function resetSelectedGradeNode() {}
 function deleteSelectedGradeNode() {}
})",
                QUrl()
            );
            std::unique_ptr<QObject> editor(fake_editor.create());
            require(bool(editor), "editor projection creates");
            QQmlComponent fake_interchange(&engine);
            fake_interchange.setData(
                R"(
import QtQml
QtObject {
 property string sourceName: ""
 property string processVersion: ""
 property string errorText: ""
 property string applyErrorText: ""
 property bool canApply: false
 property var compatibilityWarnings: []
 property var mappedAdjustments: []
 property var invalidFields: []
 property var ignoredFields: []
 property bool recipeReading: false
 property bool recipeReady: false
 property bool recipeAdaptationPartial: false
 property bool recipeAdaptationRunning: false
 property bool recipeCanAdapt: false
 property bool recipeCanApply: false
 property bool recipeCanApplyAvailableNodes: false
 property bool recipeCanRetryFailed: false
 property bool recipeExportBusy: false
 property string recipeApplyErrorText: ""
 property string recipeErrorText: ""
 property string recipeExportErrorText: ""
 property string recipeLabel: ""
 property string recipeSourceName: ""
 property int recipeGradeNodeCount: 0
 property int recipePortableMaskCount: 0
 property int recipeSemanticCompletedCount: 0
 property int recipeSemanticFailedCount: 0
 property int recipeSemanticItemCount: 0
 property var recipeSemanticItems: []
 property var recipeWarnings: []
 signal shadowRecipeReady()
 signal shadowRecipeExported()
})",
                QUrl()
            );
            std::unique_ptr<QObject> interchange(fake_interchange.create());
            require(bool(interchange), "interchange projection creates");
            QQmlComponent component(
                &engine,
                QUrl(QStringLiteral(
                    "qrc:/qt/qml/Shadow/SharedGradeLinkContract/qml/SharedGradeLinkPanel.qml"
                ))
            );
            if (component.isError())
                qCritical() << component.errors();
            const QVariantList targets{
                QVariantMap{{"photoId", "source"}, {"sourcePath", "/isolated/source.dng"}},
                QVariantMap{
                    {"photoId", "a"},
                    {"sourcePath", "/isolated/a.dng"},
                    {"title", "Target A"}
                },
                QVariantMap{{"photoId", "a"}, {"sourcePath", "/isolated/a.dng"}},
            };
            std::unique_ptr<QObject> object(component.createWithInitialProperties({
                {"linkController", QVariant::fromValue(controller.get())},
                {"editor", QVariant::fromValue(editor.get())},
                {"targets", targets},
            }));
            require(bool(object), "packaged shared link panel creates");
            auto* theme = engine.singletonInstance<QObject*>(
                qmlTypeId("Shadow.SharedGradeLinkContract", 1, 0, "Theme")
            );
            require(theme, "shared theme is packaged");
            theme->setProperty("mode", dark ? 2 : 1);
            auto* panel = qobject_cast<QQuickItem*>(object.get());
            QQuickWindow window;
            panel->setParentItem(window.contentItem());
            panel->setSize(QSizeF(700, 48));
            window.resize(700, 650);
            window.show();
            auto* dialog = object->findChild<QObject*>("sharedGradeLinkDialog");
            require(dialog, "dialog is packaged");
            for (int width : {360, 700}) {
                window.resize(width, 650);
                panel->setWidth(width);
                require(
                    QMetaObject::invokeMethod(controller.get(), "requestPublication"),
                    "publication entry invokes"
                );
                QTest::qWait(50);
                require(
                    dialog->property("visible").toBool(),
                    "publication request opens input dialog"
                );
                require(dialog->property("modal").toBool(), "input is modal");
                require(
                    dialog->property("frozenTargets").toList().size() == 1,
                    "target metadata excludes source and duplicates"
                );
                require(
                    dialog->property("width").toDouble() <= width - 40,
                    "dialog fits narrow and wide windows"
                );
                auto* content = dialog->property("contentItem").value<QQuickItem*>();
                require(content, "shared link dialog has content");
                check_bounds(content, content);
                const auto expected_title =
                    QCoreApplication::translate("SharedGradeLinkDialog", "Publish shared node");
                require(
                    dialog->property("title").toString() == expected_title,
                    "dialog title follows current language"
                );
                QMetaObject::invokeMethod(dialog, "close");
                QTest::qWait(40);
            }
            window.resize(700, 650);
            panel->setWidth(700);
            QMetaObject::invokeMethod(controller.get(), "requestPublication");
            QTest::qWait(50);
            object->setProperty(
                "targets",
                QVariantList{QVariantMap{{"photoId", "b"}, {"sourcePath", "/isolated/b.dng"}}}
            );
            click(window, object.get(), "sharedRelativePositionMasksOption");
            click(window, object.get(), "applySharedGradeLinksButton");
            require(
                controller->property("publishCount").toInt() == 1,
                "pointer publishes exactly once"
            );
            require(
                controller->property("usedSpatialMasks").toBool(),
                "spatial masks require explicit choice"
            );
            require(
                controller->property("appliedTargets").toList().first().toMap().value("photoId")
                    == "a",
                "selection changes do not retarget the frozen request"
            );
            require(!dialog->property("modal").toBool(), "results are nonmodal");
            click(window, object.get(), "closeSharedGradeLinkDialogButton");
            require(controller->property("busy").toBool(), "dismissing results does not stop work");
            click(window, object.get(), "sharedGradeLinkResultsButton");
            require(dialog->property("visible").toBool(), "pointer reopens retained results");
            click(window, object.get(), "stopSharedGradeLinksButton");
            require(
                controller->property("cancelCount").toInt() == 1,
                "stop has explicit controller authority"
            );
            click(window, object.get(), "retrySharedGradeLinksButton");
            require(
                controller->property("retryCount").toInt() == 1,
                "pointer retries retained failure state"
            );
            if (has_chinese) {
                if (translated)
                    app.removeTranslator(&chinese);
                else
                    app.installTranslator(&chinese);
                engine.retranslate();
                QTest::qWait(40);
                require(
                    dialog->property("title").toString()
                        == QCoreApplication::translate(
                            "SharedGradeLinkDialog",
                            "Shared node results"
                        ),
                    "same object tree retranslates live"
                );
                if (translated)
                    app.installTranslator(&chinese);
                else
                    app.removeTranslator(&chinese);
                engine.retranslate();
            }
            const auto evidence = qEnvironmentVariable("SHADOW_SHARED_GRADE_LINK_UI_EVIDENCE");
            if (!evidence.isEmpty())
                require(
                    window.grabWindow().save(QDir(evidence).filePath(
                        QStringLiteral("shared-link-%1-%2.png")
                            .arg(translated ? "zh" : "en", dark ? "dark" : "light")
                    )),
                    "shared link UI evidence saves"
                );
            QMetaObject::invokeMethod(dialog, "close");
            QTest::qWait(40);

            QQmlComponent menu_component(
                &engine,
                QUrl(QStringLiteral(
                    "qrc:/qt/qml/Shadow/SharedGradeLinkContract/qml/PrecisionGradeNodeMenus.qml"
                ))
            );
            if (menu_component.isError())
                qCritical() << menu_component.errors();
            std::unique_ptr<QObject> menu(menu_component.createWithInitialProperties({
                {"editor", QVariant::fromValue(editor.get())},
                {"interchangeController", QVariant::fromValue(interchange.get())},
                {"sharedGradeLinkController", QVariant::fromValue(controller.get())},
            }));
            require(bool(menu), "packaged shared node menu creates");
            auto* menu_item = qobject_cast<QQuickItem*>(menu.get());
            menu_item->setParentItem(window.contentItem());
            menu_item->setSize(QSizeF(224, 50));
            const QVariant target = QVariantMap{
                {"shared", true},
                {"rawLabel", "Shared tone"},
                {"sharedFollowHead", true},
                {"sharedLocallyModified", false},
                {"sharedUpdateAvailable", false}
            };
            auto open_menu = [&] {
                require(
                    QMetaObject::invokeMethod(
                        menu.get(),
                        "openContext",
                        Q_ARG(QVariant, QVariant::fromValue(menu_item)),
                        Q_ARG(QVariant, QVariant(0)),
                        Q_ARG(QVariant, target)
                    ),
                    "node context opens"
                );
                QTest::qWait(40);
            };
            open_menu();
            click(window, menu.get(), "syncSharedNodeAction");
            require(controller->property("syncCount").toInt() == 1, "menu routes explicit refresh");
            open_menu();
            click(window, menu.get(), "freezeSharedNodeAction");
            require(
                editor->property("followCount").toInt() == 1
                    && !editor->property("followed").toBool(),
                "menu fixes the revision explicitly"
            );
            open_menu();
            click(window, menu.get(), "unlinkSharedNodeAction");
            require(editor->property("unlinkCount").toInt() == 1, "menu routes explicit unlink");
        }
        if (translated)
            app.removeTranslator(&chinese);
    }
    return 0;
}
