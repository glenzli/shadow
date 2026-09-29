pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: picker

    required property var workspace

    width: 292
    padding: 8
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function presentAt(x, y) {
        workspace.controller.refreshSharedGradeNodes()
        picker.x = Math.max(8, Math.min(Number(x),
            workspace.width - picker.width - 8))
        picker.y = Math.max(8, Math.min(Number(y),
            workspace.height - picker.height - 8))
        picker.open()
    }

    function presentFrom(sourceItem) {
        const position = sourceItem.mapToItem(
            workspace, sourceItem.width - picker.width, sourceItem.height + 6)
        presentAt(position.x, position.y)
    }

    background: Rectangle {
        color: Theme.panelRaised
        radius: Theme.controlRadius
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: Column {
        spacing: 4

        Label {
            width: parent.width
            leftPadding: 8
            rightPadding: 8
            topPadding: 6
            bottomPadding: 8
            text: qsTr("APPLY SHARED NODE · %L1 PHOTOS").arg(
                picker.workspace.selectedPhotoCount)
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            font.weight: Font.DemiBold
            font.letterSpacing: 0.7
        }

        Label {
            width: parent.width
            leftPadding: 8
            rightPadding: 8
            topPadding: 4
            bottomPadding: 8
            visible: picker.workspace.controller.sharedGradeNodes.length === 0
            text: qsTr("No shared Grade Nodes yet")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
        }

        ListView {
            id: sharedBatchList
            width: parent.width
            height: Math.min(contentHeight, 296)
            visible: count > 0
            clip: true
            spacing: 2
            model: picker.workspace.controller.sharedGradeNodes

            delegate: Rectangle {
                id: sharedBatchRow
                required property var modelData
                width: sharedBatchList.width
                height: 38
                radius: Theme.compactControlRadius
                color: sharedBatchMouse.containsMouse
                    ? Theme.buttonGhostHover : Theme.transparent

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 8

                    ShadowIcon {
                        source: "qrc:/icons/grade-node.svg"
                        color: Theme.accent
                        size: 15
                    }

                    Label {
                        Layout.fillWidth: true
                        text: String(sharedBatchRow.modelData.label)
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSection
                        elide: Text.ElideRight
                    }

                    Label {
                        text: qsTr("V%1").arg(
                            Number(sharedBatchRow.modelData.revisionNumber))
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontCaption
                    }
                }

                MouseArea {
                    id: sharedBatchMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        picker.workspace.controller.applySharedGradeNode(
                            String(sharedBatchRow.modelData.layerId),
                            picker.workspace.batchSelectionTargets())
                        picker.close()
                    }
                }
            }
        }
    }
}
