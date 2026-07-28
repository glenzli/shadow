pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: membershipDialog

    required property var controller
    required property var manualAlbums
    required property real hostWidth

    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(336, Math.max(0, membershipDialog.hostWidth - 40))
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    padding: 16

    property var targets: []

    function openTargets(selectedTargets) {
        if (!selectedTargets || selectedTargets.length === 0 || membershipDialog.manualAlbums.length === 0 || membershipDialog.controller.libraryAlbumsBusy)
            return;
        membershipDialog.targets = selectedTargets;
        membershipDialog.open();
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
            text: qsTr("Add to Manual Album")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSection
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Add %L1 selected photos to an album.").arg(membershipDialog.targets.length)
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        ListView {
            id: manualAlbumPicker
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 192)
            visible: count > 0
            clip: true
            spacing: 4
            model: membershipDialog.manualAlbums

            delegate: ShadowButton {
                required property var modelData
                width: manualAlbumPicker.width
                compact: true
                text: String(modelData.name)
                enabled: !membershipDialog.controller.libraryAlbumsBusy
                onClicked: {
                    membershipDialog.controller.addPhotosToManualLibraryAlbum(String(modelData.id), membershipDialog.targets);
                    membershipDialog.close();
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: membershipDialog.manualAlbums.length === 0
            text: qsTr("Create a Manual Album first, then add photos here.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true

            Item {
                Layout.fillWidth: true
            }

            ShadowButton {
                compact: true
                text: qsTr("Cancel")
                onClicked: membershipDialog.close()
            }
        }
    }
}
