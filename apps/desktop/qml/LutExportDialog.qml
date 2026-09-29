pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Dialog {
    id: dialog
    required property var controller
    property bool selectedOnly: false
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(620, Math.max(0, (parent ? parent.width : 668) - 48))
    height: Math.min(650, Math.max(0, (parent ? parent.height : 698) - 48))
    modal: true
    title: qsTr("Export grading as LUT")
    closePolicy: Popup.CloseOnEscape
    padding: 20

    function present(selectedOnly) {
        if (!controller)
            return
        dialog.selectedOnly = selectedOnly
        lossConsent.checked = false
        gridSize.currentIndex = 1
        controller.prepare(selectedOnly)
        open()
    }
    onClosed: { if (controller) controller.cancel() }

    background: Rectangle {
        color: Theme.panel
        radius: 10
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 12
        Label {
            Layout.fillWidth: true
            text: qsTr("Linear sRGB → linear sRGB · input range 0–1")
            color: Theme.textPrimary
            font.bold: true
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Captures the current grading. RAW development, display rendering, and colors outside the input range are not included. Other applications must use the declared color space.")
            color: Theme.textSecondary
            wrapMode: Text.WordWrap
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: 8
                Label {
                    text: qsTr("Included nodes")
                    color: Theme.textPrimary
                    font.bold: true
                }
                Repeater {
                    model: dialog.controller ? dialog.controller.includedNodes : []
                    Label {
                        required property string modelData
                        Layout.fillWidth: true
                        text: modelData
                        color: Theme.textSecondary
                        wrapMode: Text.WordWrap
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: dialog.controller && dialog.controller.includedNodes.length === 0
                    text: qsTr("There are no enabled, unmasked Grade Nodes to export.")
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                }
                Label {
                    visible: dialog.controller && dialog.controller.omissions.length > 0
                    text: qsTr("Omitted from this LUT")
                    color: Theme.textPrimary
                    font.bold: true
                }
                Repeater {
                    model: dialog.controller ? dialog.controller.omissions : []
                    Label {
                        required property var modelData
                        Layout.fillWidth: true
                        text: modelData.nodeLabel.length > 0
                            ? modelData.nodeLabel + " · " + modelData.reason : modelData.reason
                        color: Theme.textSecondary
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
        CheckBox {
            id: lossConsent
            objectName: "lutExportLossConsent"
            Layout.fillWidth: true
            visible: dialog.controller && dialog.controller.omissions.length > 0
            enabled: dialog.controller && !dialog.controller.busy
            text: qsTr("Export the included color adjustments with these omissions")
        }
        RowLayout {
            Label { text: qsTr("Grid size"); color: Theme.textPrimary }
            ShadowComboBox {
                id: gridSize
                objectName: "lutExportGridSize"
                model: ["17 × 17 × 17", "33 × 33 × 33", "65 × 65 × 65"]
                currentIndex: 1
                enabled: dialog.controller && !dialog.controller.busy
                onActivated: dialog.controller.prepare(dialog.selectedOnly)
            }
            BusyIndicator {
                running: dialog.controller && dialog.controller.busy
                visible: running
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
            }
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.controller && dialog.controller.ready
            text: dialog.controller ? dialog.controller.measurementText : ""
            color: Theme.textPrimary
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.controller && dialog.controller.ready
            text: qsTr("Measured errors cover interpolation of the included operations only. They do not measure omitted adjustments or guarantee accuracy for every color.")
            color: Theme.textMuted
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: dialog.controller ? dialog.controller.errorText : ""
            color: Theme.errorText
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.controller && dialog.controller.savedPath.length > 0
            text: dialog.controller ? qsTr("Saved: %1").arg(dialog.controller.savedPath) : ""
            color: Theme.textSecondary
            wrapMode: Text.WrapAnywhere
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ShadowButton {
                text: dialog.controller && dialog.controller.busy ? qsTr("Cancel") : qsTr("Close")
                onClicked: dialog.close()
            }
            ShadowButton {
                objectName: "lutExportBakeButton"
                text: qsTr("Generate LUT")
                enabled: dialog.controller && dialog.controller.canBake
                    && (!lossConsent.visible || lossConsent.checked)
                onClicked: dialog.controller.bake([17, 33, 65][gridSize.currentIndex])
            }
            ShadowButton {
                objectName: "lutExportSaveButton"
                text: qsTr("Save LUT…")
                variant: ShadowButton.Primary
                enabled: dialog.controller && dialog.controller.ready && !dialog.controller.busy
                onClicked: outputFile.open()
            }
        }
    }
    FileDialog {
        id: outputFile
        title: qsTr("Save grading LUT")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "cube"
        nameFilters: [qsTr("3D LUT (*.cube)")]
        onAccepted: dialog.controller.save(selectedFile)
    }
}
