pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Page-level composition only.  The canvas owns mutually dependent viewport
// interaction; the inspector owns adjustment controls; this workspace owns the
// grade-node column and cross-panel wiring.
Item {
    id: precision

    required property var editor
    required property var lutLibrary
    required property var captureMetadata
    signal openLutLibraryRequested()
    signal openOpticsProfileLibraryRequested()
    signal returnToReviewRequested()

    readonly property color panel: Theme.panel
    readonly property color panelRaised: Theme.panelRaised
    readonly property color border: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textSecondary: Theme.textSecondary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent

    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: precision.visible && precision.editor.active
            && precision.editor.canUndo && !precision.editor.stateBusy
        onActivated: precision.editor.undo()
    }

    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: precision.visible && precision.editor.active
            && precision.editor.canRedo && !precision.editor.stateBusy
        onActivated: precision.editor.redo()
    }

    Shortcut {
        sequence: "Y"
        enabled: precision.visible && precision.editor.active
        onActivated: {
            if (precisionCanvas.comparisonActive)
                precisionCanvas.comparisonActive = false
            else
                precisionCanvas.activateComparison(precisionCanvas.comparisonMode)
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: Math.max(220, Math.min(252, precision.width * 0.19))
            Layout.fillHeight: true
            color: precision.panel
            border.width: 0

            Rectangle {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 1
                color: precision.border
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
                        color: precision.textSecondary
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.35
                    }
                    Label {
                        text: qsTr("%L1 / %L2")
                            .arg(precision.editor.gradeNodes.length).arg(16)
                        color: precision.textMuted
                        font.pixelSize: 10
                    }
                    ShadowIconButton {
                        id: addGradeNodeButton
                        source: "qrc:/icons/node-add.svg"
                        variant: ShadowIconButton.Secondary
                        foregroundColor: precision.accent
                        enabled: precision.editor.canAddGradeNode
                        toolTipText: qsTr("Add a neutral Grade Node after the selection")
                        accessibleName: qsTr("Add Grade Node")
                        onClicked: precision.editor.addGradeNode()
                    }
                }

                ListView {
                    id: gradeNodeList
                    objectName: "gradeNodeList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: precision.editor.gradeNodes
                    spacing: 6
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        id: gradeNodeRow
                        required property int index
                        required property var modelData

                        readonly property bool selected:
                            gradeNodeRow.index
                                === precision.editor.selectedGradeNodeIndex

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
                                color: gradeNodeRow.selected
                                    ? Theme.accentSurface : Theme.surfaceSubtle
                                border.width: gradeNodeRow.selected ? 1 : 0
                                border.color: Theme.accentBorder
                                Label {
                                    anchors.centerIn: parent
                                    text: String(gradeNodeRow.index + 1).padStart(2, "0")
                                    color: gradeNodeRow.selected
                                        ? precision.accent : precision.textMuted
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
                                    color: gradeNodeRow.modelData.enabled
                                        ? precision.textPrimary : precision.textSecondary
                                    font.pixelSize: 11
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: gradeNodeRow.modelData.enabled
                                        ? qsTr("LOCAL GRADE · ENABLED")
                                        : qsTr("LOCAL GRADE · BYPASSED")
                                    color: gradeNodeRow.modelData.enabled
                                        ? (gradeNodeRow.selected
                                            ? Theme.accentTextMuted
                                            : precision.textMuted)
                                        : Theme.textMuted
                                    font.pixelSize: 9
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 0.2
                                    elide: Text.ElideRight
                                }
                            }

                            Switch {
                                id: rowEnabledSwitch
                                Layout.preferredWidth: 36
                                Layout.preferredHeight: 22
                                checked: gradeNodeRow.modelData.enabled
                                enabled: precision.editor.active && !precision.editor.stateBusy
                                Accessible.name: checked
                                    ? qsTr("Bypass %1").arg(gradeNodeRow.modelData.label)
                                    : qsTr("Enable %1").arg(gradeNodeRow.modelData.label)
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                ToolTip.text: checked
                                    ? qsTr("Bypass Grade Node; preserve all adjustments")
                                    : qsTr("Enable Grade Node")
                                onClicked: {
                                    precision.editor.selectGradeNode(gradeNodeRow.index)
                                    precision.editor.gradeNodeEnabled = checked
                                }
                                indicator: Rectangle {
                                    implicitWidth: 34
                                    implicitHeight: 18
                                    x: (rowEnabledSwitch.width - width) / 2
                                    y: (rowEnabledSwitch.height - height) / 2
                                    radius: height / 2
                                    color: rowEnabledSwitch.checked
                                        ? Theme.switchOnSurface : Theme.switchOffSurface
                                    border.color: rowEnabledSwitch.checked
                                        ? Theme.switchOnBorder : Theme.switchOffBorder
                                    Rectangle {
                                        width: 12
                                        height: 12
                                        y: 3
                                        x: rowEnabledSwitch.checked ? parent.width - width - 3 : 3
                                        radius: width / 2
                                        color: rowEnabledSwitch.checked
                                            ? precision.accent : precision.textMuted
                                    }
                                }
                                contentItem: Item {}
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            anchors.rightMargin: 44
                            cursorShape: Qt.PointingHandCursor
                            onClicked: precision.editor.selectGradeNode(
                                gradeNodeRow.index)
                        }
                    }

                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 5

                    Item { Layout.fillWidth: true }

                    ShadowIconButton {
                        id: copyGradeNodeButton
                        source: "qrc:/icons/duplicate.svg"
                        toolTipText: qsTr("Duplicate selected Grade Node")
                        accessibleName: toolTipText
                        enabled: precision.editor.hasSelectedGradeNode
                            && precision.editor.canAddGradeNode
                        onClicked: precision.editor.duplicateSelectedGradeNode()
                    }
                    ShadowIconButton {
                        id: deleteGradeNodeButton
                        source: "qrc:/icons/trash.svg"
                        toolTipText: qsTr("Delete selected Grade Node")
                        accessibleName: toolTipText
                        enabled: precision.editor.canDeleteGradeNode
                        onClicked: precision.editor.deleteSelectedGradeNode()
                    }
                    ShadowIconButton {
                        id: moveGradeNodeUpButton
                        source: "qrc:/icons/move-up.svg"
                        enabled: precision.editor.canMoveGradeNodeUp
                        toolTipText: qsTr("Move selected Grade Node up")
                        accessibleName: toolTipText
                        onClicked: precision.editor.moveSelectedGradeNode(
                            precision.editor.selectedGradeNodeIndex - 1
                        )
                    }
                    ShadowIconButton {
                        id: moveGradeNodeDownButton
                        source: "qrc:/icons/move-down.svg"
                        enabled: precision.editor.canMoveGradeNodeDown
                        toolTipText: qsTr("Move selected Grade Node down")
                        accessibleName: toolTipText
                        onClicked: precision.editor.moveSelectedGradeNode(
                            precision.editor.selectedGradeNodeIndex + 1
                        )
                    }

                    Item { Layout.fillWidth: true }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: precision.border
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Grade Nodes execute from top to bottom. Each node contains a complete, non-destructive grade.")
                    color: Theme.textSubtle
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    lineHeight: 1.35
                }
            }
        }

        PrecisionCanvas {
            id: precisionCanvas
            Layout.fillWidth: true
            Layout.fillHeight: true
            editor: precision.editor
        }

        PrecisionInspector {
            Layout.preferredWidth: Math.max(304, Math.min(348, precision.width * 0.24))
            Layout.fillHeight: true
            editor: precision.editor
            lutLibrary: precision.lutLibrary
            captureMetadata: precision.captureMetadata
            displayedHistogram: precisionCanvas.displayedHistogram
            displayingBefore: precisionCanvas.displayingBefore
            readyPreviewGeneration: precisionCanvas.readyPreviewGeneration
            previewFrameReady: precisionCanvas.previewFrameReady
            comparisonActive: precisionCanvas.comparisonActive
            workspaceWidth: precision.width
            panel: precision.panel
            panelRaised: precision.panelRaised
            panelBorder: precision.border
            textPrimary: precision.textPrimary
            textSecondary: precision.textSecondary
            textMuted: precision.textMuted
            accent: precision.accent
            onOpenLutLibraryRequested: precision.openLutLibraryRequested()
            onOpenOpticsProfileLibraryRequested:
                precision.openOpticsProfileLibraryRequested()
        }
    }

    RecipeRecoveryPopup {
        editor: precision.editor
        textPrimary: precision.textPrimary
        borderColor: precision.border
        panelRaised: precision.panelRaised
        errorBorder: Theme.errorBorder
        errorText: Theme.errorText
        onReturnToReviewRequested: precision.returnToReviewRequested()
    }
}
