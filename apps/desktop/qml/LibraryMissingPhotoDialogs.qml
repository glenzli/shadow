pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Stable recovery owner for a virtualized Library card whose original source
// is currently unreachable. Card delegates pass value snapshots only; the
// dialogs never retain a delegate reference while a file picker is open. A
// selected folder is searched recursively and adopted only after one exact
// content match; no standalone-file source is created.
Item {
    id: root

    required property var controller

    property string pendingPhotoId: ""
    property string pendingLocationId: ""
    property string pendingTitle: ""
    property string pendingSourcePath: ""
    property url pendingFolder: ""

    function openRelinkFolderDialog() {
        // Native macOS dialogs can ignore an open request made in the same
        // pointer event that closes a modal Popup. Defer one event turn.
        Qt.callLater(() => relinkFolderDialog.open())
    }

    function presentMissing(photoId, locationId, title, sourcePath) {
        pendingPhotoId = String(photoId)
        pendingLocationId = String(locationId)
        pendingTitle = String(title)
        pendingSourcePath = String(sourcePath)
        missingSourcePopup.open()
    }

    function relink(photoId, locationId, title, sourcePath) {
        pendingPhotoId = String(photoId)
        pendingLocationId = String(locationId)
        pendingTitle = String(title)
        pendingSourcePath = String(sourcePath)
        openRelinkFolderDialog()
    }

    function confirmRemoval(photoId, title, sourcePath) {
        pendingPhotoId = String(photoId)
        pendingLocationId = ""
        pendingTitle = String(title)
        pendingSourcePath = String(sourcePath)
        removeDialog.open()
    }

    Popup {
        id: missingSourcePopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(500, parent.width - 40)
        padding: 18
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.warningBorder
        }

        contentItem: ColumnLayout {
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Rectangle {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    radius: Theme.compactControlRadius
                    color: Theme.warningSurface
                    border.width: 1
                    border.color: Theme.warningBorder

                    Label {
                        anchors.centerIn: parent
                        text: "!"
                        color: Theme.warningText
                        font.pixelSize: 18
                        font.weight: Font.Bold
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Original file not found")
                        color: Theme.textPrimary
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.pendingTitle
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                        elide: Text.ElideMiddle
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("The photo and its edits stay in the Library. Choose the folder that now contains the original, and Shadow will search it recursively.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: root.pendingSourcePath
                color: Theme.textSubtle
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideMiddle
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Danger
                    text: qsTr("REMOVE FROM LIBRARY")
                    enabled: root.pendingPhotoId.length > 0
                    onClicked: {
                        missingSourcePopup.close()
                        removeDialog.open()
                    }
                }

                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    text: qsTr("CANCEL")
                    onClicked: missingSourcePopup.close()
                }

                ShadowButton {
                    objectName: "missingSourceFolderButton"
                    compact: true
                    variant: ShadowButton.Primary
                    text: qsTr("ADD OR LOCATE FOLDER")
                    enabled: root.pendingPhotoId.length > 0
                        && !Boolean(root.controller.sourceRelinkBusy)
                        && !Boolean(root.controller.scanning)
                    onClicked: {
                        missingSourcePopup.close()
                        root.openRelinkFolderDialog()
                    }
                }
            }
        }
    }

    FolderDialog {
        id: relinkFolderDialog
        title: qsTr("Choose the folder containing %1").arg(root.pendingTitle)
        onAccepted: {
            root.pendingFolder = selectedFolder
            relinkConfirmDialog.open()
        }
    }

    Dialog {
        id: relinkConfirmDialog
        anchors.centerIn: parent
        width: Math.min(500, parent.width - 40)
        modal: true
        title: qsTr("Search this folder for the original?")
        standardButtons: Dialog.Cancel | Dialog.Ok

        onAccepted: root.controller.relinkUnavailableSourceLocation(
            root.pendingLocationId, root.pendingFolder)

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow will search the selected folder and its subfolders, reconnect the matching original, then add and scan the selected folder.")
                color: Theme.textPrimary
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: root.pendingFolder.toString()
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideMiddle
            }
        }
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
