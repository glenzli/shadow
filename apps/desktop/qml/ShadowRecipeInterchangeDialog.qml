pragma ComponentBehavior: Bound
pragma Translator: "ShadowRecipeInterchangeDialog"

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Dialog {
    id: dialog

    required property var editor
    required property var interchangeController
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(640, Math.max(0, parent.width - 48))
    height: Math.min(700, Math.max(0, parent.height - 48))
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 0

    function chooseImportFile() {
        interchangeController.clearShadowRecipe()
        recipeFileDialog.open()
    }

    function chooseExportFile() {
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
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.interchangeController.recipeSourceName
                    color: Theme.textMuted
                    font.pixelSize: 10
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
                Layout.topMargin: 16
                text: qsTr("Import replaces the current Grade Node list in one undoable step. Source development, repairs, AI completion, Liquify, and crop remain attached to this photo.")
                color: Theme.textSecondary
                font.pixelSize: 11
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
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("%1 Grade Nodes · %2 portable masks")
                            .arg(dialog.interchangeController.recipeGradeNodeCount)
                            .arg(dialog.interchangeController.recipePortableMaskCount)
                        color: Theme.textSecondary
                        font.pixelSize: 11
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
                    font.pixelSize: 10
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
                    font.pixelSize: 9
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
                            font.pixelSize: 10
                            wrapMode: Text.Wrap
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
                font.pixelSize: 10
                wrapMode: Text.Wrap
            }
        }
    }

    footer: Rectangle {
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
                text: qsTr("Cancel")
                variant: ShadowButton.Secondary
                onClicked: dialog.close()
            }
            ShadowButton {
                objectName: "replaceShadowRecipeGradeNodesButton"
                text: qsTr("Replace Grade Nodes")
                variant: ShadowButton.Primary
                enabled: dialog.interchangeController.recipeCanApply
                onClicked: {
                    if (dialog.interchangeController.applyShadowRecipe())
                        dialog.close()
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
                    selectedFile, dialog.editor.title)) {
                exportErrorDialog.open()
            }
        }
    }

    Dialog {
        id: exportErrorDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(430, Math.max(0, parent.width - 48))
        modal: true
        title: qsTr("Shadow Recipe was not exported")
        standardButtons: Dialog.Ok
        Label {
            width: parent.width
            text: dialog.interchangeController.recipeExportErrorText
            color: Theme.errorText
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
}
