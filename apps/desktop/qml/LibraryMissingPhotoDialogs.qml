pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Stable recovery owner for a virtualized Library card whose original source
// is currently unreachable. Card delegates pass value snapshots only; the
// dialogs never retain a delegate reference while a file picker is open.
Item {
    id: root

    required property var controller

    property string pendingPhotoId: ""
    property string pendingLocationId: ""
    property string pendingTitle: ""
    property string pendingSourcePath: ""

    function relink(photoId, locationId, title, sourcePath) {
        pendingPhotoId = String(photoId)
        pendingLocationId = String(locationId)
        pendingTitle = String(title)
        pendingSourcePath = String(sourcePath)
        relinkFileDialog.open()
    }

    function confirmRemoval(photoId, title, sourcePath) {
        pendingPhotoId = String(photoId)
        pendingLocationId = ""
        pendingTitle = String(title)
        pendingSourcePath = String(sourcePath)
        removeDialog.open()
    }

    FileDialog {
        id: relinkFileDialog
        title: qsTr("Locate original for %1").arg(root.pendingTitle)
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Photo files (*)")]
        onAccepted: root.controller.relinkUnavailableSourceLocation(
            root.pendingLocationId, selectedFile)
    }

    Dialog {
        id: removeDialog
        anchors.centerIn: parent
        width: Math.min(460, parent.width - 40)
        modal: true
        title: qsTr("Remove missing photo from Library?")
        standardButtons: Dialog.Cancel | Dialog.Ok

        onAccepted: root.controller.removeUnavailablePhotoFromLibrary(
            root.pendingPhotoId, root.pendingTitle)

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("%1 will no longer appear in the Library. The original file and Shadow’s non-destructive edits are not deleted.")
                    .arg(root.pendingTitle)
                color: Theme.textPrimary
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: root.pendingSourcePath
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideMiddle
            }
        }
    }
}
