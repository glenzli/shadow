#include <QGuiApplication>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTranslator>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "AI completion panel: " << message << '\n';
        std::exit(1);
    }
}
QQuickItem* visualItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems()) {
        if (auto* result = visualItem(child, name))
            return result;
    }
    return nullptr;
}
QQuickItem* item(QObject* root, const char* name) {
    auto* result = root->findChild<QQuickItem*>(QString::fromLatin1(name));
    if (!result) {
        auto* visualRoot = qobject_cast<QQuickItem*>(root);
        if (auto* window = qobject_cast<QQuickWindow*>(root))
            visualRoot = window->contentItem();
        if (visualRoot)
            result = visualItem(visualRoot, QString::fromLatin1(name));
    }
    require(result != nullptr, name);
    return result;
}
void checkBounds(QQuickItem* current, QQuickItem* panel) {
    if (!current->isVisible())
        return;
    // Check the visual tree, including the slider value and repeated cards.
    // Focus rings may extend two pixels beyond their control.
    const QRectF rect = current->mapRectToItem(panel, current->boundingRect());
    if (rect.width() > 0 && (rect.left() < -2.1 || rect.right() > panel->width() + 2.1)) {
        qWarning() << current << rect << "panel width" << panel->width();
        require(false, "visible content must stay inside the panel");
    }
    for (auto* child : current->childItems())
        checkBounds(child, panel);
}
void click(QQuickWindow* window, QObject* root, const char* name) {
    auto* target = item(root, name);
    require(target->isVisible() && target->isEnabled(), "action must be reachable");
    QTest::mouseClick(
        window,
        Qt::LeftButton,
        Qt::NoModifier,
        target->mapToScene(target->boundingRect().center()).toPoint()
    );
    QTest::qWait(10);
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTranslator chinese;
    require(chinese.load(QStringLiteral(SHADOW_COMPLETION_TRANSLATION)), "Chinese catalog loads");
    for (const bool translated : {false, true}) {
        if (translated)
            app.installTranslator(&chinese);
        QQmlEngine engine;
        QQmlComponent editorComponent(&engine);
        editorComponent.setData(
            R"QML(
import QtQml
QtObject {
    property bool active: true
    property bool stateBusy: false
    property bool imageCompletionExecutionAllowed: true
    property bool imageCompletionEraseMode: false
    property bool imageCompletionBusy: false
    property bool imageCompletionHasCandidate: false
    property bool imageCompletionCanGenerate: true
    property real imageCompletionBrushRadius: 0.04
    property real imageCompletionSelectionExpansion: 0
    property var imageCompletionBrushPoints: [1, 2]
    property var imageCompletionRegions: []
    property bool liquifyNodeMaterialized: false
    property bool liquifyNodeEnabled: true
    property int undoCount: 0
    property int clearCount: 0
    property int generateCount: 0
    property int cancelCount: 0
    property int applyCount: 0
    property int retryCount: 0
    property int strengthEditCount: 0
    property int beginCount: 0
    property int endCount: 0
    property string beginKey: ""
    property string endKey: ""
    function beginParameterEdit(key) { ++beginCount; beginKey = key }
    function endParameterEdit(key) { ++endCount; endKey = key }
    function setImageCompletionRegionStrength(index, value) {
        const next = imageCompletionRegions.map(region => Object.assign({}, region))
        next[index].strength = value
        imageCompletionRegions = next
        ++strengthEditCount
    }
    function setImageCompletionRegionEnabled(index, enabled) {
        const next = imageCompletionRegions.map(region => Object.assign({}, region))
        next[index].enabled = enabled
        imageCompletionRegions = next
    }
    function undoImageCompletionStroke() { ++undoCount }
    function clearImageCompletionSelection() { ++clearCount }
    function generateImageCompletion() { ++generateCount }
    function cancelImageCompletion() { ++cancelCount }
    function applyImageCompletionCandidate() { ++applyCount }
    function retryImageCompletion() { ++retryCount }
}
)QML",
            QUrl()
        );
        std::unique_ptr<QObject> editor(editorComponent.create());
        require(bool(editor), "editor fixture loads");
        QQmlComponent component(&engine);
        component.setData(
            R"QML(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "qrc:/qt/qml/Shadow/CompletionContract/qml"
ApplicationWindow {
    id: window
    required property var editor
    width: 280
    height: 900
    visible: true
    ScrollView {
        id: viewport
        anchors.fill: parent
        contentWidth: availableWidth
        ColumnLayout {
            width: viewport.availableWidth
            PrecisionAiCompletionTools {
                objectName: "completionTools"
                Layout.fillWidth: true
                Layout.margins: 12
                editor: window.editor
                authoring: true
            }
        }
    }
}
)QML",
            QUrl()
        );
        std::unique_ptr<QObject> object(
            component.createWithInitialProperties({{"editor", QVariant::fromValue(editor.get())}})
        );
        if (!object)
            qWarning() << component.errors();
        require(bool(object), "packaged panel loads in a scroll view");
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        auto* panel = item(object.get(), "completionTools");
        QSignalSpy exit(panel, SIGNAL(exitRequested()));
        QSignalSpy start(panel, SIGNAL(startRequested()));
        QSignalSpy refresh(panel, SIGNAL(refreshRequested(int)));
        const QVariantList regions{QVariantMap{
            {"enabled", true},
            {"strength", 1.0},
            {"preGrade", false},
            {"provider", "local-provider-with-a-long-identity"},
            {"modelBuild", "local-model-build-with-a-long-identity"}
        }};
        // Empty, painted, generating, candidate, liquify warning and accepted
        // regions must all fit; English and Chinese use the real translations.
        for (const int width : {232, 280, 304, 348}) {
            window->setWidth(width);
            for (int state = 0; state < 7; ++state) {
                panel->setProperty("authoring", state != 6);
                editor->setProperty("imageCompletionBusy", state == 2);
                editor->setProperty("imageCompletionHasCandidate", state == 3);
                editor->setProperty("liquifyNodeMaterialized", state == 4);
                editor->setProperty(
                    "imageCompletionRegions",
                    state >= 5 ? regions : QVariantList{}
                );
                editor->setProperty(
                    "imageCompletionBrushPoints",
                    state == 0 ? QVariantList{} : QVariantList{1}
                );
                QTest::qWait(25);
                const auto origin = panel->mapToScene({0, 0});
                require(
                    origin.x() >= 11.9 && origin.x() + panel->width() <= width - 11.9,
                    "content keeps both inspector margins"
                );
                checkBounds(panel, panel);
            }
        }
        panel->setProperty("authoring", true);
        editor->setProperty("imageCompletionRegions", QVariantList{});
        QTest::qWait(25);
        click(window, object.get(), "imageCompletionEraseButton");
        require(editor->property("imageCompletionEraseMode").toBool(), "erase mode is selected");
        click(window, object.get(), "imageCompletionPaintButton");
        require(!editor->property("imageCompletionEraseMode").toBool(), "paint mode is restored");
        click(window, object.get(), "imageCompletionUndoButton");
        click(window, object.get(), "imageCompletionClearButton");
        require(
            editor->property("undoCount").toInt() == 1
                && editor->property("clearCount").toInt() == 1,
            "compact selection actions each fire once"
        );
        click(window, object.get(), "imageCompletionGenerateButton");
        require(editor->property("generateCount").toInt() == 1, "generate remains explicit");
        auto* expansion = item(object.get(), "imageCompletionSelectionExpansion");
        require(expansion->isEnabled(), "painted selection can be expanded");
        auto* expansionInput = item(expansion, "shadowSliderAccessibleInput");
        QTest::mouseClick(
            window,
            Qt::LeftButton,
            Qt::NoModifier,
            expansionInput
                ->mapToScene({expansionInput->width() * 0.5, expansionInput->height() / 2})
                .toPoint()
        );
        QTest::qWait(25);
        require(
            editor->property("imageCompletionSelectionExpansion").toDouble() > 0.0,
            "expansion slider edits the controller property"
        );
        editor->setProperty("imageCompletionBusy", true);
        QTest::qWait(25);
        require(
            !item(object.get(), "imageCompletionPaintButton")->isEnabled()
                && !item(object.get(), "imageCompletionBrushSize")->isEnabled()
                && !expansion->isEnabled(),
            "generation keeps its submitted selection stable"
        );
        click(window, object.get(), "imageCompletionGenerateButton");
        require(
            editor->property("cancelCount").toInt() == 1 && exit.count() == 1,
            "busy primary action still cancels and exits"
        );
        editor->setProperty("imageCompletionBusy", false);
        editor->setProperty("imageCompletionHasCandidate", true);
        QTest::qWait(25);
        click(window, object.get(), "imageCompletionApplyButton");
        click(window, object.get(), "imageCompletionRetryButton");
        click(window, object.get(), "imageCompletionCancelButton");
        require(
            editor->property("applyCount").toInt() == 1
                && editor->property("retryCount").toInt() == 1 && exit.count() == 2,
            "candidate apply, retry and cancel remain separate reachable actions"
        );
        panel->setProperty("authoring", false);
        QTest::qWait(25);
        click(window, object.get(), "beginImageCompletionButton");
        require(start.count() == 1, "accepted-region panel can start another selection");

        editor->setProperty("imageCompletionRegions", regions);
        QTest::qWait(25);
        QPointer<QQuickItem> strength(item(object.get(), "imageCompletionRegionStrength_0"));
        auto* input = item(strength, "shadowSliderAccessibleInput");
        const auto point = [&](double fraction) {
            return input->mapToScene({input->width() * fraction, input->height() / 2}).toPoint();
        };
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point(0.95));
        for (double fraction : {0.8, 0.65, 0.5, 0.35}) {
            QTest::mouseMove(window, point(fraction), 20);
            QTest::qWait(10);
            require(!strength.isNull(), "model feedback must not destroy a dragged region slider");
        }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point(0.35));
        QTest::qWait(25);
        require(
            editor->property("strengthEditCount").toInt() >= 3,
            "continuous drag publishes successive values despite model feedback"
        );
        require(
            editor->property("beginCount").toInt() == 1 && editor->property("endCount").toInt() == 1
                && editor->property("beginKey").toString() == "image_completion/region/0/strength"
                && editor->property("beginKey") == editor->property("endKey"),
            "one region drag uses one photo-local undo gesture"
        );
        click(window, object.get(), "imageCompletionRegionEnabled_0");
        require(
            !editor->property("imageCompletionRegions").toList()[0].toMap()["enabled"].toBool(),
            "accepted region can be bypassed"
        );
        click(window, object.get(), "imageCompletionRegionRefresh_0");
        require(
            refresh.count() == 1 && refresh.at(0).at(0).toInt() == 0,
            "accepted region can request regeneration of its original selection"
        );
        editor->setProperty("stateBusy", true);
        require(!strength->isEnabled(), "busy editor still protects accepted regions");
        editor->setProperty("stateBusy", false);
        panel->setProperty("authoring", true);
        QTest::qWait(25);
        require(strength.isNull(), "authoring shows a summary instead of disabled region controls");
    }
    std::cout << "AI completion panel: 56 localized width/state cases and pointer actions passed\n";
    return 0;
}
