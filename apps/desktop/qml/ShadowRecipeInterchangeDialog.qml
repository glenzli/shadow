pragma ComponentBehavior: Bound
pragma Translator: "ShadowRecipeInterchangeDialog"

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ShadowDialog {
    id: dialog

    required property var editor
    required property var interchangeController
    property bool exportSelectedOnly: false
    onClosed: {
        if (interchangeController.recipeReading === true)
            interchangeController.clearShadowRecipe()
    }
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(640, Math.max(0, (parent ? parent.width : 688) - 48))
    height: Math.min(700, Math.max(0, (parent ? parent.height : 748) - 48))
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 0

    readonly property bool hasSemanticRecipe:
        interchangeController.recipeSemanticItemCount > 0
    readonly property int semanticResolvedCount: Math.min(
        interchangeController.recipeSemanticItemCount,
        interchangeController.recipeSemanticCompletedCount
            + interchangeController.recipeSemanticFailedCount
    )
    readonly property real semanticProgress: interchangeController.recipeSemanticItemCount > 0
        ? semanticResolvedCount / interchangeController.recipeSemanticItemCount : 0

    function semanticStateLabel(state) {
        switch (state) {
        case "waiting": return qsTr("Waiting")
        case "preparing": return qsTr("Preparing")
        case "grounding": return qsTr("Finding subject")
        case "segmenting": return qsTr("Creating mask")
        case "completed": return qsTr("Ready")
        case "notFound": return qsTr("Not found")
        case "unavailable": return qsTr("Unavailable")
        case "failed": return qsTr("Failed")
        case "cancelled": return qsTr("Cancelled")
        default: return qsTr("Waiting")
        }
    }

    function semanticStateColor(state) {
        if (state === "completed")
            return Theme.successText
        if (state === "notFound" || state === "unavailable"
                || state === "failed")
            return Theme.errorText
        if (state === "cancelled")
            return Theme.textMuted
        return Theme.accentTextMuted
    }

    function semanticItemDetail(item) {
        if (item.state === "unavailable")
            return qsTr("Check Infer Runtime and the local models, then retry.")
        if (item.errorText && item.errorText.length > 0)
            return item.errorText
        if (item.query && item.query.length > 0)
            return qsTr("Semantic subject: %1").arg(item.query)
        return ""
    }

    function applyRecipe() {
        if (hasSemanticRecipe && interchangeController.recipeAdaptationPartial)
            return false
        return interchangeController.applyShadowRecipe()
    }

    function startAdaptation() {
        return interchangeController.startShadowRecipeAdaptation()
    }

    function cancelAdaptation() {
        interchangeController.cancelShadowRecipeAdaptation()
    }

    function retryFailedItems() {
        return interchangeController.retryFailedShadowRecipeItems()
    }

    function applyAvailableNodes() {
        return interchangeController.applyAvailableShadowRecipeNodes()
    }

    function chooseImportFile() {
        interchangeController.clearShadowRecipe()
        recipeFileDialog.open()
    }

    function chooseExportFile(selectedOnly) {
        exportSelectedOnly = selectedOnly === true
        interchangeController.clearShadowRecipe()
        recipeExportFileDialog.open()
    }

    background: Rectangle {
        radius: 10
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    header: Rectangle {
        implicitHeight: 58
        color: Theme.transparent
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 12
            spacing: 8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: qsTr("Import Shadow Recipe")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.interchangeController.recipeSourceName
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    elide: Text.ElideMiddle
                }
            }
            ShadowIconButton {
                source: "qrc:/icons/close.svg"
                toolTipText: qsTr("Close")
                accessibleName: qsTr("Close Shadow Recipe import")
                onClicked: dialog.close()
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.border
        }
    }

    contentItem: ScrollView {
        id: previewScroll
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: previewScroll.availableWidth
            spacing: 14
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                visible: dialog.interchangeController.recipeReading === true
                text: qsTr("Reading Recipe and validating bundled LUTs…")
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.topMargin: 16
                text: qsTr("Import replaces the current Grade Node list in one undoable step. Source development, repairs, AI completion, Liquify, and crop remain attached to this photo.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSection
                wrapMode: Text.Wrap
            }

            Rectangle {
                visible: dialog.interchangeController.recipeReady
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                implicitHeight: summaryColumn.implicitHeight + 22
                radius: 7
                color: Theme.panel
                border.width: 1
                border.color: Theme.border
                ColumnLayout {
                    id: summaryColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 5
                    Label {
                        visible: dialog.interchangeController.recipeLabel.length > 0
                        Layout.fillWidth: true
                        text: dialog.interchangeController.recipeLabel
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("%1 Grade Nodes · %2 portable masks")
                            .arg(dialog.interchangeController.recipeGradeNodeCount)
                            .arg(dialog.interchangeController.recipePortableMaskCount)
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                }
            }

            Rectangle {
                visible: dialog.interchangeController.recipeErrorText.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                implicitHeight: recipeErrorLabel.implicitHeight + 20
                radius: 7
                color: Theme.dangerSurface
                border.width: 1
                border.color: Theme.errorBorder
                Label {
                    id: recipeErrorLabel
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    text: dialog.interchangeController.recipeErrorText
                    color: Theme.errorText
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.Wrap
                }
            }

            ColumnLayout {
                visible: dialog.interchangeController.recipeWarnings.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                spacing: 7
                Label {
                    text: qsTr("IMPORT NOTES")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.35
                }
                Repeater {
                    model: dialog.interchangeController.recipeWarnings
                    delegate: Rectangle {
                        id: warningRow
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: warningLabel.implicitHeight + 18
                        radius: 6
                        color: Theme.warningSurface
                        border.width: 1
                        border.color: Theme.warningBorder
                        Label {
                            id: warningLabel
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            text: warningRow.modelData
                            color: Theme.warningText
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            ColumnLayout {
                id: semanticAdaptationSection
                objectName: "semanticRecipeAdaptationSection"
                visible: dialog.hasSemanticRecipe
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("SEMANTIC MASK ADAPTATION")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontCaption
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.35
                    }
                    Label {
                        text: qsTr("%1 of %2 ready")
                            .arg(dialog.interchangeController.recipeSemanticCompletedCount)
                            .arg(dialog.interchangeController.recipeSemanticItemCount)
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontMeta
                    }
                }

                ProgressBar {
                    objectName: "semanticRecipeProgress"
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: dialog.semanticProgress
                    indeterminate: dialog.interchangeController.recipeAdaptationRunning
                        && dialog.semanticResolvedCount === 0
                    Accessible.name: qsTr("Semantic mask adaptation progress")
                }

                Label {
                    visible: dialog.interchangeController.recipeAdaptationPartial
                    Layout.fillWidth: true
                    text: qsTr("Some Grade Nodes could not be adapted. Retry those items, or explicitly import only the available nodes. Unavailable nodes are excluded as complete Grade Nodes.")
                    color: Theme.warningText
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.Wrap
                }

                Repeater {
                    model: dialog.interchangeController.recipeSemanticItems
                    delegate: Rectangle {
                        id: semanticItemRow
                        required property var modelData
                        objectName: "semanticRecipeItem-" + modelData.itemId
                        Layout.fillWidth: true
                        implicitHeight: semanticItemColumn.implicitHeight + 18
                        radius: 6
                        color: Theme.panel
                        border.width: 1
                        border.color: Theme.border

                        ColumnLayout {
                            id: semanticItemColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 3

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: semanticItemRow.modelData.nodeLabel
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontSection
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: dialog.semanticStateLabel(
                                        semanticItemRow.modelData.state)
                                    color: dialog.semanticStateColor(
                                        semanticItemRow.modelData.state)
                                    font.pixelSize: Theme.fontMeta
                                    font.weight: Font.DemiBold
                                }
                            }
                            Label {
                                visible: text.length > 0
                                Layout.fillWidth: true
                                text: dialog.semanticItemDetail(
                                    semanticItemRow.modelData)
                                color: semanticItemRow.modelData.state === "unavailable"
                                    ? Theme.errorText : Theme.textMuted
                                font.pixelSize: Theme.fontMeta
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }

            Label {
                visible: dialog.interchangeController.recipeApplyErrorText.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.bottomMargin: 16
                text: dialog.interchangeController.recipeApplyErrorText
                color: Theme.errorText
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.Wrap
            }
        }
    }

    footer: Rectangle {
        objectName: "recipeDialogFooter"
        implicitHeight: 58
        color: Theme.transparent
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.border
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 8
            Item { Layout.fillWidth: true }
            ShadowButton {
                objectName: "cancelShadowRecipeButton"
                text: dialog.interchangeController.recipeAdaptationRunning
                    ? qsTr("Cancel adaptation") : qsTr("Cancel")
                variant: ShadowButton.Secondary
                onClicked: {
                    if (dialog.interchangeController.recipeAdaptationRunning)
                        dialog.cancelAdaptation()
                    else
                        dialog.close()
                }
            }
            ShadowButton {
                objectName: "retryFailedShadowRecipeButton"
                visible: dialog.hasSemanticRecipe
                    && dialog.interchangeController.recipeCanRetryFailed
                text: qsTr("Retry failed items")
                variant: ShadowButton.Secondary
                enabled: !dialog.interchangeController.recipeAdaptationRunning
                onClicked: dialog.retryFailedItems()
            }
            ShadowButton {
                objectName: "applyAvailableShadowRecipeButton"
                visible: dialog.hasSemanticRecipe
                    && dialog.interchangeController.recipeCanApplyAvailableNodes
                text: qsTr("Import available nodes")
                variant: ShadowButton.Secondary
                enabled: !dialog.interchangeController.recipeAdaptationRunning
                onClicked: {
                    if (dialog.applyAvailableNodes())
                        dialog.close()
                }
            }
            ShadowButton {
                objectName: "applyShadowRecipeButton"
                visible: !dialog.hasSemanticRecipe
                    || !dialog.interchangeController.recipeAdaptationPartial
                text: dialog.hasSemanticRecipe
                    ? (dialog.interchangeController.recipeCanAdapt
                        ? qsTr("Adapt and Import")
                        : dialog.interchangeController.recipeAdaptationRunning
                            ? qsTr("Adapting…")
                            : qsTr("Import adapted Recipe"))
                    : qsTr("Replace Grade Nodes")
                variant: ShadowButton.Primary
                enabled: dialog.hasSemanticRecipe
                    ? (dialog.interchangeController.recipeCanAdapt
                        || (!dialog.interchangeController.recipeAdaptationPartial
                            && dialog.interchangeController.recipeCanApply))
                        && !dialog.interchangeController.recipeAdaptationRunning
                    : dialog.interchangeController.recipeCanApply
                onClicked: {
                    if (dialog.hasSemanticRecipe
                            && dialog.interchangeController.recipeCanAdapt) {
                        dialog.startAdaptation()
                    } else if (dialog.applyRecipe()) {
                        dialog.close()
                    }
                }
            }
        }
    }

    FileDialog {
        id: recipeFileDialog
        title: qsTr("Choose a Shadow Recipe")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("Shadow Recipes (*.shadowrecipe)"),
            qsTr("JSON documents (*.json)"),
            qsTr("All files (*)")
        ]
        onAccepted: {
            dialog.interchangeController.previewShadowRecipe(selectedFile)
            dialog.open()
        }
    }

    FileDialog {
        id: recipeExportFileDialog
        title: qsTr("Export Shadow Recipe")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "shadowrecipe"
        nameFilters: [qsTr("Shadow Recipes (*.shadowrecipe)")]
        onAccepted: {
            if (!dialog.interchangeController.exportShadowRecipe(
                    selectedFile, dialog.editor.title, dialog.exportSelectedOnly)) {
                exportErrorDialog.open()
            } else {
                exportProgressDialog.open()
            }
        }
    }

    Connections {
        target: dialog.interchangeController
        ignoreUnknownSignals: true
        function onRecipeExportStateChanged() {
            if (!dialog.interchangeController.recipeExportBusy) {
                exportProgressDialog.close()
                if (dialog.interchangeController.recipeExportErrorText.length > 0)
                    exportErrorDialog.open()
            }
        }
    }

    ShadowDialog {
        id: exportProgressDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(430, Math.max(0, (parent ? parent.width : 478) - 48))
        modal: true
        closePolicy: Popup.NoAutoClose
        title: qsTr("Exporting Shadow Recipe")
        contentItem: Label {
            text: qsTr("Packaging the captured Recipe and its LUT resources…")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            wrapMode: Text.WordWrap
        }
    }

    ShadowDialog {
        id: exportErrorDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(430, Math.max(0, (parent ? parent.width : 478) - 48))
        modal: true
        title: qsTr("Shadow Recipe was not exported")
        standardButtons: Dialog.Ok
        contentItem: Label {
            text: dialog.interchangeController.recipeExportErrorText
            color: Theme.errorText
            font.pixelSize: Theme.fontSection
            wrapMode: Text.Wrap
        }
    }
}
