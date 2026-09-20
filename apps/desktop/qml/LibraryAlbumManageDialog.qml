pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: manageDialog

    required property var controller
    required property real hostWidth

    signal deleteRequested(string albumId, string albumName)

    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(342, Math.max(0, manageDialog.hostWidth - 40))
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    padding: 16

    property string albumId: ""
    property string albumName: ""
    property string albumKind: "manual"

    function openAlbum(id, name, kind) {
        manageDialog.albumId = id;
        manageDialog.albumName = name;
        manageDialog.albumKind = kind;
        manageDialog.open();
    }

    onOpened: {
        albumRenameInput.text = albumName;
        albumRenameInput.selectAll();
        albumRenameInput.forceActiveFocus();
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: qsTr("Manage album")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSection
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: manageDialog.albumKind === "smart" ? qsTr("Filtered Album") : qsTr("Manual Album")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
        }

        ShadowTextField {
            id: albumRenameInput
            Layout.fillWidth: true
            selectByMouse: true
            onAccepted: renameAlbumButton.clicked()
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ShadowButton {
                compact: true
                variant: ShadowButton.Danger
                text: qsTr("Delete")
                enabled: !manageDialog.controller.libraryAlbumsBusy
                onClicked: {
                    manageDialog.close();
                    manageDialog.deleteRequested(manageDialog.albumId, manageDialog.albumName);
                }
            }

            Item {
                Layout.fillWidth: true
            }

            ShadowButton {
                compact: true
                text: qsTr("Cancel")
                onClicked: manageDialog.close()
            }

            ShadowButton {
                id: renameAlbumButton
                compact: true
                variant: ShadowButton.Primary
                text: qsTr("Rename")
                enabled: albumRenameInput.text.trim().length > 0 && !manageDialog.controller.libraryAlbumsBusy
                onClicked: {
                    manageDialog.controller.renameLibraryAlbum(manageDialog.albumId, albumRenameInput.text);
                    manageDialog.close();
                }
            }
        }
    }
}
