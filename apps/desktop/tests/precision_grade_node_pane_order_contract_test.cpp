#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTranslator>
#include <QVariantList>
#include <cstdlib>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        qCritical() << message;
        std::exit(1);
    }
}

QQuickItem* row(QObject* root, const char* name) {
    auto* result = root->findChild<QQuickItem*>(QString::fromLatin1(name));
    require(result != nullptr, name);
    return result;
}

qreal top(QQuickItem* item, QQuickItem* pane) {
    return item->mapToItem(pane, QPointF{0, 0}).y();
}

QVariantMap region(bool preGrade) {
    return {{QStringLiteral("preGrade"), preGrade}, {QStringLiteral("enabled"), true}};
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
    property bool canAddGradeNode: true
    property bool canDeleteGradeNode: false
    property bool canMoveGradeNodeUp: false
    property bool canMoveGradeNodeDown: false
    property bool hasSelectedGradeNode: false
    property bool gradeNodeEnabled: true
    property var gradeNodes: [{label: "Adjustment", enabled: true, shared: false,
        sharedRevisionNumber: 0, hasLocalMask: false, localMaskComponentCount: 0,
        localMaskKind: 0}]
    property int selectedGradeNodeIndex: 0
    property string selectedRecipeNodeKind: "grade"
    property bool canvasNodeMaterialized: false
    property bool canvasNodeEnabled: true
    property bool liquifyNodeMaterialized: false
    property bool liquifyNodeEnabled: true
    property bool retouchNodeMaterialized: false
    property bool retouchNodeEnabled: true
    property var paint: QtObject { property var layers: [] }
    property var imageCompletionRegions: []
    property bool imageCompletionNodeMaterialized: imageCompletionRegions.length > 0
    property bool imageCompletionNodeEnabled: true
    property bool foundationEnabled: true
    property bool foundationSelected: false
    property bool rawDenoiseNodeMaterialized: true
    property bool rawDenoiseNodeVisible: true
    property bool rawDenoiseSelected: false
    property bool foundationAiDenoiseBusy: false
    property bool foundationAiDenoiseEnabled: true
    property bool foundationAiDenoiseCanApply: true
    property int foundationAiDenoiseAmount: 100
    function selectImageCompletionNode() { selectedRecipeNodeKind = "completion" }
}
)QML",
            QUrl()
        );
        std::unique_ptr<QObject> editor(editorComponent.create());
        require(bool(editor), "editor fixture loads");

        QQmlComponent paneComponent(&engine);
        paneComponent.setData(
            R"QML(
import QtQuick
import QtQuick.Controls
import "qrc:/qt/qml/Shadow/GradeNodePaneOrderContract/qml"
ApplicationWindow {
    id: window
    required property var editor
    width: 360
    height: 900
    visible: true
    PrecisionGradeNodePane {
        id: pane
        objectName: "nodePane"
        anchors.fill: parent
        editor: window.editor
        interchangeController: null
        panel: "#f8fafc"
        panelRaised: "#ffffff"
        borderColor: "#dce4ef"
        textPrimary: "#202b38"
        textSecondary: "#526579"
        textMuted: "#718198"
        accent: "#1e78d4"
    }
}
)QML",
            QUrl()
        );
        std::unique_ptr<QObject> object(paneComponent.createWithInitialProperties(
            {{"editor", QVariant::fromValue(editor.get())}}
        ));
        if (!object)
            qWarning() << paneComponent.errors();
        require(bool(object), "packaged node pane loads");

        auto* pane = row(object.get(), "nodePane");
        auto* legacy = row(object.get(), "legacyImageCompletionNodeRow");
        auto* current = row(object.get(), "imageCompletionNodeRow");
        auto* grades = row(object.get(), "gradeNodeList");
        auto* foundation = row(object.get(), "foundationNodeRow");
        auto* foundationLabel = row(object.get(), "rawDevelopmentNodeLabel");
        auto* denoise = row(object.get(), "rawDenoiseNodeRow");
        QTest::qWait(25);
        require(!legacy->isVisible() && !current->isVisible(), "no phantom completion row");
        require(
            foundationLabel->property("text").toString()
                == (translated ? QStringLiteral("RAW 显影") : QStringLiteral("RAW Development")),
            "source stage is distinguished from the editable Grade controls"
        );

        editor->setProperty("selectedRecipeNodeKind", "completion");
        QTest::qWait(25);
        require(current->isVisible() && !legacy->isVisible(), "new selection enters pre-grade row");

        editor->setProperty("selectedRecipeNodeKind", "grade");
        editor->setProperty("imageCompletionRegions", QVariantList{region(false), region(false)});
        QTest::qWait(25);
        require(legacy->isVisible() && !current->isVisible(), "older regions stay after grades");
        require(top(legacy, pane) < top(grades, pane), "older row is above adjustment stack");
        require(
            legacy->property("nodeStatus")
                .toString()
                .contains(translated ? QStringLiteral("调色后") : QStringLiteral("AFTER GRADES")),
            "older stage is labeled in both languages"
        );

        editor->setProperty("imageCompletionRegions", QVariantList{region(true)});
        QTest::qWait(25);
        require(!legacy->isVisible() && current->isVisible(), "new region uses pre-grade row");
        require(
            top(current, pane) > top(grades, pane) && top(current, pane) < top(foundation, pane)
                && top(foundation, pane) < top(denoise, pane),
            "new row is between grades and foundation, above RAW denoise"
        );
        require(
            current->property("nodeStatus")
                .toString()
                .contains(translated ? QStringLiteral("调色前") : QStringLiteral("BEFORE GRADES")),
            "new stage is labeled in both languages"
        );

        editor->setProperty("imageCompletionRegions", QVariantList{region(false), region(true)});
        QTest::qWait(25);
        require(legacy->isVisible() && current->isVisible(), "mixed recipe shows both stages");
        require(
            pane->property("preGradeCompletionCount").toInt() == 1
                && pane->property("legacyCompletionCount").toInt() == 1,
            "each stage counts only its own regions"
        );
        editor->setProperty("imageCompletionNodeEnabled", false);
        QTest::qWait(25);
        require(
            legacy->property("nodeStatus")
                    .toString()
                    .contains(translated ? QStringLiteral("已旁路") : QStringLiteral("BYPASSED"))
                && current->property("nodeStatus")
                       .toString()
                       .contains(
                           translated ? QStringLiteral("已旁路") : QStringLiteral("BYPASSED")
                       ),
            "shared bypass is reflected at both stages"
        );
    }
    return 0;
}
