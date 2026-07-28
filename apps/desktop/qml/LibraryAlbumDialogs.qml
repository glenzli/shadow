pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var controller
    required property bool hasActiveLibraryFilter
    required property var manualAlbums

    function openCreate() {
        albumCreatePopup.open()
    }

    function openMembership(targets) {
        if (!targets || targets.length === 0
                || root.manualAlbums.length === 0
                || root.controller.libraryAlbumsBusy)
            return
        albumMembershipPopup.targets = targets
        albumMembershipPopup.open()
    }

    function openManage(albumId, albumName, albumKind) {
        albumManagePopup.albumId = albumId
        albumManagePopup.albumName = albumName
        albumManagePopup.albumKind = albumKind
        albumManagePopup.open()
    }

    Popup {
        id: albumCreatePopup
        parent: Overlay.overlay
        modal: true
        focus: true
        width: Math.min(330, root.width - 40)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        padding: 16
        property string creationKind: "manual"

        onOpened: {
            if (!root.hasActiveLibraryFilter)
                creationKind = "manual"
            albumNameInput.text = ""
            albumNameInput.forceActiveFocus()
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
                text: albumCreatePopup.creationKind === "condition"
                    ? qsTr("A Condition Album keeps the current Library conditions as a reusable view.")
                    : qsTr("An Album holds only the photos you add to it.")
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
                    selected: albumCreatePopup.creationKind === "manual"
                    onClicked: albumCreatePopup.creationKind = "manual"
                }

                ShadowButton {
                    Layout.fillWidth: true
                    compact: true
                    text: qsTr("Condition")
                    selected: albumCreatePopup.creationKind === "condition"
                    enabled: root.hasActiveLibraryFilter
                    toolTipText: enabled
                        ? qsTr("Save the current Library conditions")
                        : qsTr("Set at least one Library condition first")
                    accessibleName: toolTipText
                    onClicked: albumCreatePopup.creationKind = "condition"
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

                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: albumCreatePopup.close()
                }

                ShadowButton {
                    id: createAlbumButton
                    compact: true
                    variant: ShadowButton.Primary
                    text: qsTr("Create")
                    enabled: albumNameInput.text.trim().length > 0
                        && !root.controller.libraryAlbumsBusy
                    onClicked: {
                        if (albumCreatePopup.creationKind === "condition")
                            root.controller.createSmartLibraryAlbum(albumNameInput.text)
                        else
                            root.controller.createManualLibraryAlbum(albumNameInput.text)
                        albumCreatePopup.close()
                    }
                }
            }
        }
    }

    Popup {
        id: albumMembershipPopup
        parent: Overlay.overlay
        modal: true
        focus: true
        width: Math.min(336, root.width - 40)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        padding: 16
        property var targets: []

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
                text: qsTr("Add %L1 selected photos to an album.").arg(
                    albumMembershipPopup.targets.length)
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
                model: root.manualAlbums

                delegate: ShadowButton {
                    required property var modelData
                    width: manualAlbumPicker.width
                    compact: true
                    text: String(modelData.name)
                    enabled: !root.controller.libraryAlbumsBusy
                    onClicked: {
                        root.controller.addPhotosToManualLibraryAlbum(
                            String(modelData.id),
                            albumMembershipPopup.targets)
                        albumMembershipPopup.close()
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.manualAlbums.length === 0
                text: qsTr("Create a Manual Album first, then add photos here.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: albumMembershipPopup.close()
                }
            }
        }
    }

    Popup {
        id: albumManagePopup
        parent: Overlay.overlay
        modal: true
        focus: true
        width: Math.min(342, root.width - 40)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        padding: 16
        property string albumId: ""
        property string albumName: ""
        property string albumKind: "manual"

        onOpened: {
            albumRenameInput.text = albumName
            albumRenameInput.selectAll()
            albumRenameInput.forceActiveFocus()
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
                text: albumManagePopup.albumKind === "smart"
                    ? qsTr("Filtered Album") : qsTr("Manual Album")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }

            TextField {
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
                    enabled: !root.controller.libraryAlbumsBusy
                    onClicked: {
                        albumDeletePopup.albumId = albumManagePopup.albumId
                        albumDeletePopup.albumName = albumManagePopup.albumName
                        albumManagePopup.close()
                        albumDeletePopup.open()
                    }
                }

                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: albumManagePopup.close()
                }

                ShadowButton {
                    id: renameAlbumButton
                    compact: true
                    variant: ShadowButton.Primary
                    text: qsTr("Rename")
                    enabled: albumRenameInput.text.trim().length > 0
                        && !root.controller.libraryAlbumsBusy
                    onClicked: {
                        root.controller.renameLibraryAlbum(
                            albumManagePopup.albumId,
                            albumRenameInput.text)
                        albumManagePopup.close()
                    }
                }
            }
        }
    }

    Popup {
        id: albumDeletePopup
        parent: Overlay.overlay
        modal: true
        focus: true
        width: Math.min(342, root.width - 40)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        padding: 16
        property string albumId: ""
        property string albumName: ""

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
                text: qsTr("Delete \u201c%1\u201d? Photos and their edits stay in the Library.").arg(
                    albumDeletePopup.albumName)
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: albumDeletePopup.close()
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Danger
                    text: qsTr("Delete album")
                    enabled: !root.controller.libraryAlbumsBusy
                    onClicked: {
                        root.controller.deleteLibraryAlbum(
                            albumDeletePopup.albumId)
                        albumDeletePopup.close()
                    }
                }
            }
        }
    }
}
