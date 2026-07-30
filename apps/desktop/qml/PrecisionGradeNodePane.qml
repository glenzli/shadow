pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The Grade Node collection surface owns navigation, ordering, enablement, and
// collection actions. Popup transactions live in PrecisionGradeNodeMenus.
Rectangle {
    id: pane

    required property var editor
    required property color panel
    required property color panelRaised
    required property color borderColor
    required property color textPrimary
    required property color textSecondary
    required property color textMuted
    required property color accent

    signal maskToolRequested

    color: pane.panel
    PrecisionGradeNodeMenus {
        id: gradeNodeMenus
        editor: pane.editor
    }

    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 1
        color: pane.borderColor
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.panelPadding
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label {
                Layout.fillWidth: true
                text: qsTr("GRADE NODES")
                color: pane.textSecondary
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }
            Label {
                text: qsTr("%L1 / %L2").arg(pane.editor.gradeNodes.length).arg(16)
                color: pane.textMuted
                font.pixelSize: 10
            }
            ShadowIconButton {
                id: addGradeNodeButton
                source: "qrc:/icons/node-add.svg"
                variant: ShadowIconButton.Secondary
                foregroundColor: pane.accent
                enabled: pane.editor.canAddGradeNode
                toolTipText: qsTr("Add a neutral Grade Node after the selection")
                accessibleName: qsTr("Add Grade Node")
                onClicked: gradeNodeMenus.openAdd(addGradeNodeButton)
            }
        }

        ListView {
            id: gradeNodeList
            objectName: "gradeNodeList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: pane.editor.gradeNodes
            spacing: 6
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: gradeNodeRow
                required property int index
                required property var modelData

                readonly property bool selected: gradeNodeRow.index === pane.editor.selectedGradeNodeIndex

                width: gradeNodeList.width
                height: 52
                radius: 6
                color: selected ? Theme.accentSurfaceQuiet : Theme.panelRaised
                border.width: selected ? 1 : 0
                border.color: Theme.accentBorder

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 8
                    spacing: 7

                    Rectangle {
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 22
                        radius: 5
                        color: gradeNodeRow.selected ? Theme.accentSurface : Theme.surfaceSubtle
                        border.width: gradeNodeRow.selected ? 1 : 0
                        border.color: Theme.accentBorder
                        Label {
                            anchors.centerIn: parent
                            text: String(gradeNodeRow.index + 1).padStart(2, "0")
                            color: gradeNodeRow.selected ? pane.accent : pane.textMuted
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: gradeNodeRow.modelData.label
                            color: gradeNodeRow.modelData.enabled ? pane.textPrimary : pane.textSecondary
                            font.pixelSize: 11
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: (gradeNodeRow.modelData.shared ? (gradeNodeRow.modelData.enabled ? qsTr("SHARED · V%1 · ENABLED").arg(gradeNodeRow.modelData.sharedRevisionNumber) : qsTr("SHARED · V%1 · BYPASSED").arg(gradeNodeRow.modelData.sharedRevisionNumber)) : (gradeNodeRow.modelData.enabled ? qsTr("LOCAL · ENABLED") : qsTr("LOCAL · BYPASSED"))) + (gradeNodeRow.modelData.hasLocalMask ? qsTr(" · NODE MASK") : "")
                            color: gradeNodeRow.modelData.enabled ? (gradeNodeRow.selected ? Theme.accentTextMuted : pane.textMuted) : Theme.textMuted
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.2
                            elide: Text.ElideRight
                        }
                    }

                    ShadowIcon {
                        visible: gradeNodeRow.modelData.shared
                        source: "qrc:/icons/shared-link.svg"
                        color: gradeNodeRow.selected ? pane.accent : pane.textMuted
                        size: 14
                    }

                    ShadowIconButton {
                        buttonSize: 30
                        iconSize: 17
                        source: gradeNodeRow.modelData.localMaskKind === 1 ? "qrc:/icons/mask-linear.svg" : gradeNodeRow.modelData.localMaskKind === 2 ? "qrc:/icons/mask-radial.svg" : gradeNodeRow.modelData.localMaskKind === 3 ? "qrc:/icons/brush.svg" : gradeNodeRow.modelData.localMaskKind === 4 ? "qrc:/icons/mask-luminance-range.svg" : gradeNodeRow.modelData.localMaskKind === 5 ? "qrc:/icons/mask-color-range.svg" : "qrc:/icons/mask-add.svg"
                        toolTipText: gradeNodeRow.modelData.hasLocalMask ? qsTr("Edit this node mask") : qsTr("Add a mask to this node")
                        accessibleName: toolTipText
                        enabled: pane.editor.active && gradeNodeRow.modelData.enabled && !pane.editor.stateBusy
                        onClicked: {
                            pane.editor.selectGradeNode(gradeNodeRow.index);
                            if (gradeNodeRow.modelData.hasLocalMask) {
                                pane.maskToolRequested();
                            } else {
                                nodeMaskCreateMenu.openFor(this, nodeMaskCreateMenu.currentNodeDestination);
                            }
                        }
                    }

                    Switch {
                        id: rowEnabledSwitch
                        Layout.preferredWidth: 36
                        Layout.preferredHeight: 22
                        checked: gradeNodeRow.modelData.enabled
                        enabled: pane.editor.active && !pane.editor.stateBusy
                        Accessible.name: checked ? qsTr("Bypass %1").arg(gradeNodeRow.modelData.label) : qsTr("Enable %1").arg(gradeNodeRow.modelData.label)
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: checked ? qsTr("Bypass Grade Node; preserve all adjustments") : qsTr("Enable Grade Node")
                        onClicked: {
                            pane.editor.selectGradeNode(gradeNodeRow.index);
                            pane.editor.gradeNodeEnabled = checked;
                        }
                        indicator: Rectangle {
                            implicitWidth: 34
                            implicitHeight: 18
                            x: (rowEnabledSwitch.width - width) / 2
                            y: (rowEnabledSwitch.height - height) / 2
                            radius: height / 2
                            color: rowEnabledSwitch.checked ? Theme.switchOnSurface : Theme.switchOffSurface
                            border.color: rowEnabledSwitch.checked ? Theme.switchOnBorder : Theme.switchOffBorder
                            Rectangle {
                                width: 12
                                height: 12
                                y: 3
                                x: rowEnabledSwitch.checked ? parent.width - width - 3 : 3
                                radius: width / 2
                                color: rowEnabledSwitch.checked ? pane.accent : pane.textMuted
                            }
                        }
                        contentItem: Item {}
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.rightMargin: 82
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: Qt.PointingHandCursor
                    onClicked: mouse => {
                        pane.editor.selectGradeNode(gradeNodeRow.index);
                        if (mouse.button === Qt.RightButton) {
                            gradeNodeMenus.openContext(gradeNodeRow, mouse.y, gradeNodeRow.modelData);
                        }
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 5

            Item {
                Layout.fillWidth: true
            }

            ShadowIconButton {
                id: copyGradeNodeButton
                source: "qrc:/icons/duplicate.svg"
                toolTipText: qsTr("Duplicate selected Grade Node")
                accessibleName: toolTipText
                enabled: pane.editor.hasSelectedGradeNode && pane.editor.canAddGradeNode
                onClicked: pane.editor.duplicateSelectedGradeNode()
            }
            ShadowIconButton {
                id: deleteGradeNodeButton
                source: "qrc:/icons/trash.svg"
                toolTipText: qsTr("Delete selected Grade Node")
                accessibleName: toolTipText
                enabled: pane.editor.canDeleteGradeNode
                onClicked: pane.editor.deleteSelectedGradeNode()
            }
            ShadowIconButton {
                id: moveGradeNodeUpButton
                source: "qrc:/icons/move-up.svg"
                enabled: pane.editor.canMoveGradeNodeUp
                toolTipText: qsTr("Move selected Grade Node up")
                accessibleName: toolTipText
                onClicked: pane.editor.moveSelectedGradeNode(pane.editor.selectedGradeNodeIndex - 1)
            }
            ShadowIconButton {
                id: moveGradeNodeDownButton
                source: "qrc:/icons/move-down.svg"
                enabled: pane.editor.canMoveGradeNodeDown
                toolTipText: qsTr("Move selected Grade Node down")
                accessibleName: toolTipText
                onClicked: pane.editor.moveSelectedGradeNode(pane.editor.selectedGradeNodeIndex + 1)
            }

            Item {
                Layout.fillWidth: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: pane.borderColor
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Nodes run top to bottom. A mask belongs to its node and this photo.")
            color: Theme.textSubtle
            wrapMode: Text.WordWrap
            font.pixelSize: 10
            lineHeight: 1.35
        }
    }

    PrecisionMaskCreateMenu {
        id: nodeMaskCreateMenu
        editor: pane.editor
        onMaskCreated: pane.maskToolRequested()
        onAiMaskRequested: pane.maskToolRequested()
        onEditExistingRequested: pane.maskToolRequested()
    }
}
