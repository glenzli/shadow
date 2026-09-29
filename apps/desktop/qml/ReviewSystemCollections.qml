pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the built-in Library collection projection and selection intents.
ColumnLayout {
    id: collections

    required property var workspace
    readonly property var systemCounts:
        workspace.controller.librarySystemCollectionCounts

    spacing: 8

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 34
        radius: 7
        color: collections.workspace.isSystemCollectionActive("all")
            ? Theme.accentSurface : Theme.transparent

        Rectangle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 3
            width: 3
            height: 18
            radius: 1.5
            color: collections.workspace.isSystemCollectionActive("all")
                ? collections.workspace.accent : Theme.transparent
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 10

            Label {
                text: qsTranslate("ReviewWorkspace", "All Photos")
                color: collections.workspace.textPrimary
            }

            Label {
                objectName: "allPhotosCount"
                Layout.fillWidth: true
                text: qsTranslate("ReviewWorkspace", "%L1").arg(
                    collections.systemCounts.available
                        ? collections.systemCounts.all
                        : collections.workspace.controller.itemCount)
                color: collections.workspace.textMuted
                horizontalAlignment: Text.AlignRight
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: collections.workspace.applySystemCollection("all")
        }
    }

    Repeater {
        model: [
            {
                id: "recent-imports",
                title: qsTranslate("ReviewWorkspace", "Recent Imports"),
                icon: "qrc:/icons/history.svg",
                count: collections.systemCounts.recentImports
            },
            {
                id: "liked",
                title: qsTranslate("ReviewWorkspace", "Liked"),
                icon: "qrc:/icons/heart.svg",
                count: collections.systemCounts.liked
            },
            {
                id: "five-star",
                title: qsTranslate("ReviewWorkspace", "5 Stars"),
                icon: "qrc:/icons/star.svg",
                count: collections.systemCounts.fiveStar
            }
        ]

        delegate: Rectangle {
            id: collectionRow

            required property var modelData
            readonly property bool selected:
                collections.workspace.isSystemCollectionActive(
                    String(modelData.id))

            Layout.fillWidth: true
            Layout.preferredHeight: 32
            radius: Theme.compactControlRadius
            color: selected
                ? Theme.accentSurface
                : collectionMouse.containsMouse
                    ? Theme.buttonGhostHover : Theme.transparent

            Rectangle {
                anchors.left: parent.left
                anchors.leftMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                width: 2
                height: 16
                radius: 1
                color: collectionRow.selected
                    ? collections.workspace.accent : Theme.transparent
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 13
                anchors.rightMargin: 9
                spacing: 7

                ShadowIcon {
                    source: String(collectionRow.modelData.icon)
                    color: String(collectionRow.modelData.id) === "liked"
                        ? Theme.likeAccent
                        : String(collectionRow.modelData.id) === "five-star"
                            ? Theme.labelYellow
                            : collectionRow.selected
                                ? collections.workspace.accent
                                : collections.workspace.textMuted
                    size: 14
                }

                Label {
                    Layout.fillWidth: true
                    text: String(collectionRow.modelData.title)
                    color: collectionRow.selected
                        ? collections.workspace.textPrimary
                        : collections.workspace.textSecondary
                    font.pixelSize: Theme.fontSection
                    elide: Text.ElideRight
                }
                Label {
                    visible: collections.systemCounts.available
                    text: qsTranslate("ReviewWorkspace", "%L1").arg(
                        collectionRow.modelData.count)
                    color: collections.workspace.textMuted
                    font.pixelSize: Theme.fontMeta
                }

            }

            MouseArea {
                id: collectionMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: collections.workspace.applySystemCollection(
                    String(collectionRow.modelData.id))
            }
        }
    }
}
