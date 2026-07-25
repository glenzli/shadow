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

    readonly property bool proxyActive: editor.active
        && precisionCanvas.visiblePreviewSource.length > 0
        && !precisionCanvas.showingFullDetail
    readonly property color panel: Theme.panel
    readonly property color panelRaised: Theme.panelRaised
    readonly property color border: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textSecondary: Theme.textSecondary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent

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
        onOpened: precision.editor.refreshSharedGradeNodes()
        background: Rectangle {
            radius: 8
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
        contentItem: Column {
            spacing: 2

            PopupAction {
                text: qsTr("New adjustment")
                onClicked: {
                    addGradeNodePopup.close()
                    precision.editor.addGradeNode()
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
                text: qsTr("SHARED NODES")
                color: Theme.textMuted
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }

            Label {
                visible: precision.editor.sharedGradeNodes.length === 0
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
                model: precision.editor.sharedGradeNodes
                delegate: PopupAction {
                    required property var modelData
                    text: modelData.label
                    onClicked: {
                        addGradeNodePopup.close()
                        precision.editor.insertSharedGradeNode(modelData.layerId)
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
                enabled: precision.editor.canAddGradeNode
                onClicked: {
                    gradeNodeContextPopup.close()
                    precision.editor.duplicateSelectedGradeNode()
                }
            }
            PopupAction {
                text: qsTr("Use linear mask")
                enabled: precision.editor.active && !precision.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    precision.editor.setSelectedLocalMask(1)
                }
            }
            PopupAction {
                text: qsTr("Use radial mask")
                enabled: precision.editor.active && !precision.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    precision.editor.setSelectedLocalMask(2)
                }
            }
            PopupAction {
                visible: gradeNodeContextPopup.targetData
                    && gradeNodeContextPopup.targetData.hasLocalMask
                text: qsTr("Remove local mask")
                enabled: precision.editor.active && !precision.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    precision.editor.setSelectedLocalMask(0)
                }
            }
            PopupAction {
                text: qsTr("Delete node")
                enabled: precision.editor.canDeleteGradeNode
                onClicked: {
                    gradeNodeContextPopup.close()
                    precision.editor.deleteSelectedGradeNode()
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
                        precision.editor.publishSelectedGradeNode(text)
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
                        precision.editor.publishSelectedGradeNode(
                            sharedNodeNameField.text)
                        sharedNodeNamePopup.close()
                    }
                }
            }
        }
    }

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
                        onClicked: {
                            const point = addGradeNodeButton.mapToItem(
                                addGradeNodePopup.parent,
                                addGradeNodeButton.width
                                    - addGradeNodePopup.width,
                                addGradeNodeButton.height + 5)
                            addGradeNodePopup.x = point.x
                            addGradeNodePopup.y = point.y
                            addGradeNodePopup.open()
                        }
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
                                    text: gradeNodeRow.modelData.shared
                                        ? (gradeNodeRow.modelData.enabled
                                            ? qsTr("SHARED · V%1 · ENABLED")
                                                .arg(gradeNodeRow.modelData.sharedRevisionNumber)
                                            : qsTr("SHARED · V%1 · BYPASSED")
                                                .arg(gradeNodeRow.modelData.sharedRevisionNumber))
                                        : (gradeNodeRow.modelData.enabled
                                            ? qsTr("LOCAL · ENABLED")
                                            : qsTr("LOCAL · BYPASSED"))
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

                            ShadowIcon {
                                visible: gradeNodeRow.modelData.shared
                                source: "qrc:/icons/shared-link.svg"
                                color: gradeNodeRow.selected
                                    ? precision.accent : precision.textMuted
                                size: 14
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
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            cursorShape: Qt.PointingHandCursor
                            onClicked: mouse => {
                                precision.editor.selectGradeNode(
                                    gradeNodeRow.index)
                                if (mouse.button === Qt.RightButton) {
                                    gradeNodeContextPopup.targetData =
                                        gradeNodeRow.modelData
                                    const point = gradeNodeRow.mapToItem(
                                        gradeNodeContextPopup.parent,
                                        gradeNodeRow.width
                                            - gradeNodeContextPopup.width,
                                        mouse.y)
                                    gradeNodeContextPopup.x = point.x
                                    gradeNodeContextPopup.y = point.y
                                    gradeNodeContextPopup.open()
                                }
                            }
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
            currentPhotoAspect: precisionCanvas.imagePixelWidth
                / Math.max(1, precisionCanvas.imagePixelHeight)
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
