pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns album loading, selection, creation, and management entry points.
ColumnLayout {
    id: albums

    required property var workspace
    required property var albumDialogs

    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 12
        Layout.bottomMargin: 2
        spacing: 6

        Label {
            Layout.fillWidth: true
            text: qsTranslate("ReviewWorkspace", "ALBUMS")
            color: albums.workspace.textMuted
            font.pixelSize: 9
            font.weight: Font.DemiBold
            font.letterSpacing: 1.1
        }

        BusyIndicator {
            Layout.preferredWidth: 14
            Layout.preferredHeight: 14
            visible: albums.workspace.controller.libraryAlbumsBusy
            running: visible
        }

        ShadowIconButton {
            source: "qrc:/icons/node-add.svg"
            buttonSize: 24
            iconSize: 15
            toolTipText: qsTranslate("ReviewWorkspace", "Create album")
            accessibleName: toolTipText
            enabled: !albums.workspace.controller.libraryAlbumsBusy
            onClicked: albums.albumDialogs.openCreate()
        }
    }

    ListView {
        id: albumList
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, 188)
        visible: count > 0
        clip: true
        spacing: 2
        model: albums.workspace.controller.libraryAlbums

        delegate: Rectangle {
            id: albumRow

            required property var modelData
            readonly property string albumId: String(modelData.id)
            readonly property bool selected:
                albums.workspace.controller.libraryAlbumId === albumId

            width: albumList.width
            height: 32
            radius: Theme.compactControlRadius
            color: selected
                ? Theme.accentSurface
                : albumMouse.containsMouse
                    ? Theme.buttonGhostHover : Theme.transparent

            Rectangle {
                anchors.left: parent.left
                anchors.leftMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                width: 2
                height: 16
                radius: 1
                color: albumRow.selected
                    ? albums.workspace.accent : Theme.transparent
            }

            RowLayout {
                z: 1
                anchors.fill: parent
                anchors.leftMargin: 11
                anchors.rightMargin: 8
                spacing: 7

                ShadowIcon {
                    source: String(albumRow.modelData.kind) === "smart"
                        ? "qrc:/icons/filter.svg"
                        : "qrc:/icons/library-manage.svg"
                    color: albumRow.selected
                        ? albums.workspace.accent
                        : albums.workspace.textMuted
                    size: 14
                }

                Label {
                    Layout.fillWidth: true
                    text: String(albumRow.modelData.name)
                    color: albumRow.selected
                        ? albums.workspace.textPrimary
                        : albums.workspace.textSecondary
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }

                Label {
                    visible: String(albumRow.modelData.kind) === "smart"
                    text: qsTranslate("ReviewWorkspace", "CONDITION")
                    color: albumRow.selected
                        ? albums.workspace.accent
                        : albums.workspace.textMuted
                    font.pixelSize: 8
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.55
                }

                ShadowIconButton {
                    visible: albumRow.selected || albumMouse.containsMouse
                    source: "qrc:/icons/settings.svg"
                    buttonSize: 22
                    iconSize: 13
                    toolTipText: qsTranslate(
                        "ReviewWorkspace", "Manage album")
                    accessibleName: toolTipText
                    enabled: !albums.workspace.controller.libraryAlbumsBusy
                    onClicked: albums.albumDialogs.openManage(
                        albumRow.albumId,
                        String(albumRow.modelData.name),
                        String(albumRow.modelData.kind))
                }
            }

            MouseArea {
                id: albumMouse
                anchors.fill: parent
                z: 0
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked:
                    albums.workspace.controller.libraryAlbumId =
                        albumRow.albumId
            }
        }
    }
}
