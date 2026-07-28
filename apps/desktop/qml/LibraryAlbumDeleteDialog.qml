pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: deleteDialog

    required property var controller
    required property real hostWidth

    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(342, Math.max(0, deleteDialog.hostWidth - 40))
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    padding: 16

    property string albumId: ""
    property string albumName: ""

    function openAlbum(id, name) {
        deleteDialog.albumId = id;
        deleteDialog.albumName = name;
        deleteDialog.open();
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.dangerBorder
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: qsTr("Delete album?")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSection
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Delete \u201c%1\u201d? Photos and their edits stay in the Library.").arg(deleteDialog.albumName)
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item {
                Layout.fillWidth: true
            }

            ShadowButton {
                compact: true
                text: qsTr("Cancel")
                onClicked: deleteDialog.close()
            }

            ShadowButton {
                compact: true
                variant: ShadowButton.Danger
                text: qsTr("Delete album")
                enabled: !deleteDialog.controller.libraryAlbumsBusy
                onClicked: {
                    deleteDialog.controller.deleteLibraryAlbum(deleteDialog.albumId);
                    deleteDialog.close();
                }
            }
        }
    }
}
