pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Popup {
    id: dialog
    required property var compositionController
    property var targets: []
    property string mode: "hdr"
    parent: Overlay.overlay
    width: Math.min(900, parent.width - 40)
    height: Math.min(760, parent.height - 40)
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    modal: true
    dim: true
    focus: true
    padding: 20
    closePolicy: compositionController.busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    function present(inputs, kind) {
        if (compositionController.busy) return
        targets = inputs
        mode = kind
        compositionController.prepare(inputs, kind)
        open()
    }
    onClosed: compositionController.cancel()
    background: Rectangle { color: Theme.panel; radius: 12; border.color: Theme.border }
    FileDialog {
        id: saveDialog
        title: qsTr("Save composite")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Linear TIFF (*.tif *.tiff)")]
        onAccepted: dialog.compositionController.save(selectedFile)
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        Label {
            text: dialog.mode === "hdr" ? qsTr("HDR merge") : qsTr("Panorama merge")
            font.pixelSize: 20
            color: Theme.textPrimary
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: dialog.mode === "hdr"
                ? qsTr("Use a RAW exposure bracket with fixed camera, aperture, ISO and white balance. Original files and their edits remain unchanged.")
                : qsTr("Use overlapping views of one scene. Photos are aligned automatically and cropped to a fully covered rectangle. Existing edits are not applied.")
        }
        Label {
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            color: Theme.textMuted
            text: dialog.targets.map(t => String(t.title || t.sourcePath || "").split("/").pop()).join(" · ")
        }
        RowLayout {
            enabled: !dialog.compositionController.busy && !dialog.compositionController.ready
            Label { text: qsTr("Source long edge"); color: Theme.textPrimary }
            ComboBox { id: resolution; model: ["2048", "4096"]; currentIndex: 1 }
            CheckBox { id: align; visible: dialog.mode === "hdr"; checked: true; text: qsTr("Auto align") }
            CheckBox { id: deghost; visible: dialog.mode === "hdr"; checked: true; text: qsTr("Reduce ghosting") }
            CheckBox { id: compensate; visible: dialog.mode === "panorama"; checked: true; text: qsTr("Match exposure") }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("The selected size bounds each source image. The result is a separate 32-bit linear TIFF that can be edited after import.")
            color: Theme.textMuted
            wrapMode: Text.WordWrap
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.surfaceSubtle
            radius: 8
            Image {
                objectName: "compositionResultPreview"
                anchors.fill: parent
                anchors.margins: 8
                source: dialog.compositionController.preview
                asynchronous: true
                fillMode: Image.PreserveAspectFit
            }
            BusyIndicator { anchors.centerIn: parent; running: dialog.compositionController.busy; visible: running }
        }
        Label { Layout.fillWidth: true; text: dialog.compositionController.status; color: Theme.textPrimary; wrapMode: Text.WordWrap }
        Label { text: dialog.compositionController.dimensions; color: Theme.textMuted; visible: text.length > 0 }
        Label { Layout.fillWidth: true; text: dialog.compositionController.error; color: Theme.dangerText; wrapMode: Text.WordWrap; visible: text.length > 0 }
        Label { Layout.fillWidth: true; text: dialog.compositionController.savedPath; color: Theme.textMuted; elide: Text.ElideMiddle; visible: text.length > 0 }
        RowLayout {
            Layout.fillWidth: true
            ShadowButton {
                text: dialog.compositionController.busy ? qsTr("Cancel composition") : qsTr("Close")
                onClicked: { dialog.compositionController.cancel(); if (!dialog.compositionController.busy) dialog.close() }
            }
            Item { Layout.fillWidth: true }
            ShadowButton {
                text: qsTr("Change settings")
                visible: dialog.compositionController.ready
                onClicked: dialog.compositionController.prepare(dialog.targets, dialog.mode)
            }
            ShadowButton {
                objectName: "startPhotoCompositionButton"
                text: qsTr("Merge")
                enabled: !dialog.compositionController.busy && !dialog.compositionController.ready
                    && dialog.compositionController.savedPath.length === 0
                onClicked: dialog.compositionController.start(Number(resolution.currentText), align.checked, deghost.checked, compensate.checked)
            }
            ShadowButton {
                objectName: "savePhotoCompositionButton"
                text: qsTr("Save and import")
                variant: ShadowButton.Primary
                enabled: dialog.compositionController.ready
                onClicked: { saveDialog.selectedFile = dialog.compositionController.suggestedDestination(); saveDialog.open() }
            }
        }
    }
}
