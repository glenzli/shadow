pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: createDialog

    required property var controller
    required property bool hasActiveLibraryFilter
    required property real hostWidth

    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(330, Math.max(0, createDialog.hostWidth - 40))
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    padding: 16

    property string creationKind: "manual"

    onOpened: {
        if (!createDialog.hasActiveLibraryFilter)
            creationKind = "manual";
        albumNameInput.text = "";
        albumNameInput.forceActiveFocus();
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
            text: qsTr("Create album")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSection
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: createDialog.creationKind === "condition" ? qsTr("A Condition Album keeps the current Library conditions as a reusable view.") : qsTr("An Album holds only the photos you add to it.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                text: qsTr("Album")
                selected: createDialog.creationKind === "manual"
                onClicked: createDialog.creationKind = "manual"
            }

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                text: qsTr("Condition")
                selected: createDialog.creationKind === "condition"
                enabled: createDialog.hasActiveLibraryFilter
                toolTipText: enabled ? qsTr("Save the current Library conditions") : qsTr("Set at least one Library condition first")
                accessibleName: toolTipText
                onClicked: createDialog.creationKind = "condition"
            }

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                text: qsTr("Smart")
                enabled: false
                toolTipText: qsTr("AI-driven Smart Albums are not available yet")
                accessibleName: toolTipText
            }
        }

        TextField {
            id: albumNameInput
            Layout.fillWidth: true
            placeholderText: qsTr("Album name")
            selectByMouse: true
            onAccepted: createAlbumButton.clicked()
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
                onClicked: createDialog.close()
            }

            ShadowButton {
                id: createAlbumButton
                compact: true
                variant: ShadowButton.Primary
                text: qsTr("Create")
                enabled: albumNameInput.text.trim().length > 0 && !createDialog.controller.libraryAlbumsBusy
                onClicked: {
                    if (createDialog.creationKind === "condition")
                        createDialog.controller.createSmartLibraryAlbum(albumNameInput.text);
                    else
                        createDialog.controller.createManualLibraryAlbum(albumNameInput.text);
                    createDialog.close();
                }
            }
        }
    }
}
