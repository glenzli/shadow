pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Dialog {
    id: dialog
    required property var pipeline
    property url destination
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(540, parent ? parent.width - 40 : 540)
    modal: true
    title: qsTr("Export edited photos")
    contentItem: ColumnLayout {
        spacing: Theme.sectionSpacing
        Label {
            Layout.fillWidth: true
            text: qsTr("Export all %1 photos. Existing files are never overwritten.").arg(dialog.pipeline.photoCount)
            color: Theme.textSecondary
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontBody
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: dialog.destination.toString().length > 0 ? decodeURIComponent(dialog.destination.toString().replace(/^file:\/\//, "")) : qsTr("Choose an output folder")
                elide: Text.ElideMiddle
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
            }
            ShadowButton { text: qsTr("Choose…"); onClicked: folderPicker.open() }
        }
        GridLayout {
            columns: 2
            columnSpacing: 16
            rowSpacing: 12
            Label { text: qsTr("Format"); color: Theme.textSecondary; font.pixelSize: Theme.fontBody }
            ShadowComboBox { id: format; model: ["JPEG", "PNG", "TIFF"]; Layout.fillWidth: true }
            Label { text: qsTr("Color space"); color: Theme.textSecondary; font.pixelSize: Theme.fontBody }
            ShadowComboBox { id: color; model: ["sRGB", "Display P3"]; Layout.fillWidth: true }
            Label { text: qsTr("JPEG quality"); visible: format.currentIndex === 0; color: Theme.textSecondary; font.pixelSize: Theme.fontBody }
            SpinBox { id: quality; from: 1; to: 100; value: 95; editable: true; visible: format.currentIndex === 0; font.pixelSize: Theme.fontBody }
            Label { text: qsTr("TIFF bit depth"); visible: format.currentIndex === 2; color: Theme.textSecondary; font.pixelSize: Theme.fontBody }
            ShadowComboBox { id: depth; model: ["8", "16"]; currentIndex: 1; visible: format.currentIndex === 2; Layout.fillWidth: true }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Full resolution · filenames end in -edited · no source metadata is copied")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.pipeline.errorText.length > 0
            text: dialog.pipeline.errorText
            wrapMode: Text.Wrap
            color: Theme.errorText
            font.pixelSize: Theme.fontBody
        }
    }
    footer: DialogButtonBox {
        ShadowButton { text: qsTr("Cancel"); onClicked: dialog.close() }
        ShadowButton {
            text: qsTr("Export")
            enabled: dialog.destination.toString().length > 0
            onClicked: {
                const options = { format: ["jpeg", "png", "tiff"][format.currentIndex],
                    quality: quality.value, bitDepth: format.currentIndex === 2 && depth.currentIndex === 1 ? 16 : 8,
                    colorSpace: color.currentIndex === 0 ? "srgb" : "display-p3", maxEdge: 0, metadataPolicy: "none" }
                if (dialog.pipeline.configureExport(dialog.destination, options)) {
                    dialog.close()
                    dialog.pipeline.complete()
                }
            }
        }
    }
    FolderDialog { id: folderPicker; title: qsTr("Choose an output folder"); onAccepted: dialog.destination = selectedFolder }
}
