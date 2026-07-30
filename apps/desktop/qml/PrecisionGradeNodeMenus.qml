pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Popups share the selected Grade Node transaction and remain independent of
// the pane layout. Callers provide only the anchor and the intended target.
Item {
    id: menus

    required property var editor

    signal cropGeometryRequested
    signal liquifyRequested

    function openAdd(anchorItem) {
        const point = anchorItem.mapToItem(
            addGradeNodePopup.parent,
            anchorItem.width - addGradeNodePopup.width,
            anchorItem.height + 5)
        addGradeNodePopup.x = point.x
        addGradeNodePopup.y = point.y
        addGradeNodePopup.open()
    }

    function openContext(anchorItem, pointerY, targetData) {
        gradeNodeContextPopup.targetData = targetData
        const point = anchorItem.mapToItem(
            gradeNodeContextPopup.parent,
            anchorItem.width - gradeNodeContextPopup.width,
            pointerY)
        gradeNodeContextPopup.x = point.x
        gradeNodeContextPopup.y = point.y
        gradeNodeContextPopup.open()
    }

    component PopupAction: Button {
        id: popupAction
        width: parent ? parent.width : 204
        implicitHeight: 32
        leftPadding: 10
        rightPadding: 10
        hoverEnabled: enabled
        background: Rectangle {
            radius: 5
            color: !popupAction.enabled ? Theme.transparent
                : popupAction.down ? Theme.buttonGhostPressed
                : popupAction.hovered ? Theme.buttonGhostHover
                : Theme.transparent
        }
        contentItem: Label {
            text: popupAction.text
            color: popupAction.enabled ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: 11
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    Popup {
        id: addGradeNodePopup
        parent: Overlay.overlay
        width: 224
        padding: 7
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: menus.editor.refreshSharedGradeNodes()
        background: Rectangle {
            radius: 8
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
        contentItem: Column {
            spacing: 2

            Label {
                width: parent.width
                leftPadding: 10
                topPadding: 6
                bottomPadding: 3
                text: qsTr("STRUCTURAL")
                color: Theme.textMuted
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }

            PopupAction {
                text: qsTr("Crop & Geometry")
                onClicked: {
                    addGradeNodePopup.close()
                    menus.editor.selectCanvasNode()
                    menus.cropGeometryRequested()
                }
            }

            PopupAction {
                text: menus.editor.liquifyNodeMaterialized
                    ? qsTr("Open Liquify")
                    : qsTr("Add Liquify")
                onClicked: {
                    addGradeNodePopup.close()
                    menus.editor.selectLiquifyNode()
                    menus.liquifyRequested()
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.border
            }

            Label {
                width: parent.width
                leftPadding: 10
                topPadding: 6
                bottomPadding: 3
                text: qsTr("ADJUSTMENTS")
                color: Theme.textMuted
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }

            PopupAction {
                text: qsTr("New adjustment")
                enabled: menus.editor.canAddGradeNode
                onClicked: {
                    addGradeNodePopup.close()
                    menus.editor.addGradeNode()
                }
            }

            Label {
                width: parent.width
                leftPadding: 10
                topPadding: 6
                bottomPadding: 3
                text: qsTr("SHARED NODES")
                color: Theme.textMuted
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }

            Label {
                visible: menus.editor.sharedGradeNodes.length === 0
                width: parent.width
                leftPadding: 10
                rightPadding: 10
                topPadding: 6
                bottomPadding: 8
                text: qsTr("No shared nodes yet")
                color: Theme.textMuted
                font.pixelSize: 10
            }

            Repeater {
                model: menus.editor.sharedGradeNodes
                delegate: PopupAction {
                    required property var modelData
                    text: modelData.label
                    enabled: menus.editor.canAddGradeNode
                    onClicked: {
                        addGradeNodePopup.close()
                        menus.editor.insertSharedGradeNode(modelData.layerId)
                    }
                }
            }
        }
    }

    Popup {
        id: gradeNodeContextPopup
        parent: Overlay.overlay
        width: 224
        padding: 7
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        property var targetData: null
        background: Rectangle {
            radius: 8
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
        contentItem: Column {
            spacing: 2
            PopupAction {
                text: gradeNodeContextPopup.targetData
                    && gradeNodeContextPopup.targetData.shared
                    ? qsTr("Update shared node…")
                    : qsTr("Share node…")
                onClicked: {
                    gradeNodeContextPopup.close()
                    const suggested = gradeNodeContextPopup.targetData
                        ? gradeNodeContextPopup.targetData.rawLabel : ""
                    sharedNodeNameField.text = suggested
                    sharedNodeNamePopup.open()
                }
            }
            PopupAction {
                text: qsTr("Duplicate as independent")
                enabled: menus.editor.canAddGradeNode
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.editor.duplicateSelectedGradeNode()
                }
            }
            PopupAction {
                text: qsTr("Reset node")
                enabled: menus.editor.active
                    && menus.editor.hasSelectedGradeNode
                    && !menus.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.editor.resetSelectedGradeNode()
                }
            }
            PopupAction {
                text: qsTr("Delete node")
                enabled: menus.editor.canDeleteGradeNode
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.editor.deleteSelectedGradeNode()
                }
            }
        }
    }

    Popup {
        id: sharedNodeNamePopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: 340
        padding: 16
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: {
            sharedNodeNameField.forceActiveFocus()
            sharedNodeNameField.selectAll()
        }
        background: Rectangle {
            radius: 10
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: qsTr("Shared Grade Node")
                color: Theme.textPrimary
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("A stable library node can be linked to many photos.")
                color: Theme.textMuted
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }
            TextField {
                id: sharedNodeNameField
                Layout.fillWidth: true
                placeholderText: qsTr("Node name")
                selectByMouse: true
                color: Theme.textPrimary
                placeholderTextColor: Theme.textPlaceholder
                background: Rectangle {
                    radius: Theme.controlRadius
                    color: Theme.panelRaised
                    border.width: sharedNodeNameField.activeFocus ? 1 : 0
                    border.color: Theme.accent
                }
                onAccepted: {
                    if (text.trim().length > 0) {
                        menus.editor.publishSelectedGradeNode(text)
                        sharedNodeNamePopup.close()
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                ShadowButton {
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Cancel")
                    onClicked: sharedNodeNamePopup.close()
                }
                ShadowButton {
                    compact: true
                    variant: ShadowButton.Primary
                    text: qsTr("Share")
                    enabled: sharedNodeNameField.text.trim().length > 0
                    onClicked: {
                        menus.editor.publishSelectedGradeNode(
                            sharedNodeNameField.text)
                        sharedNodeNamePopup.close()
                    }
                }
            }
        }
    }
}
