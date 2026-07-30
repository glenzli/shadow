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
    signal cropToolRequested
    signal liquifyToolRequested

    color: pane.panel
    PrecisionGradeNodeMenus {
        id: gradeNodeMenus
        editor: pane.editor
        onCropGeometryRequested: pane.cropToolRequested()
        onLiquifyRequested: pane.liquifyToolRequested()
    }

    component StructuralNodeRow: Rectangle {
        id: structuralRow

        required property string nodeLabel
        required property string nodeStatus
        required property string nodeGlyph
        required property bool nodeSelected

        signal activated

        Layout.fillWidth: true
        Layout.preferredHeight: 52
        radius: 6
        color: nodeSelected ? Theme.accentSurfaceQuiet : Theme.panelRaised
        border.width: nodeSelected ? 1 : 0
        border.color: Theme.accentBorder

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 7

            Rectangle {
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: 5
                color: structuralRow.nodeSelected
                    ? Theme.accentSurface : Theme.surfaceSubtle
                border.width: structuralRow.nodeSelected ? 1 : 0
                border.color: Theme.accentBorder

                Label {
                    anchors.centerIn: parent
                    text: structuralRow.nodeGlyph
                    color: structuralRow.nodeSelected
                        ? pane.accent : pane.textMuted
                    font.pixelSize: 9
                    font.weight: Font.Bold
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: structuralRow.nodeLabel
                    color: pane.textPrimary
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: structuralRow.nodeStatus
                    color: structuralRow.nodeSelected
                        ? Theme.accentTextMuted : pane.textMuted
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.2
                    elide: Text.ElideRight
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.PointingHandCursor
            onClicked: structuralRow.activated()
        }
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
                text: qsTr("NODES")
                color: pane.textSecondary
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }
            Label {
                text: qsTr("GRADES %L1 / %L2")
                    .arg(pane.editor.gradeNodes.length).arg(16)
                color: pane.textMuted
                font.pixelSize: 10
            }
            ShadowIconButton {
                id: addGradeNodeButton
                source: "qrc:/icons/node-add.svg"
                variant: ShadowIconButton.Secondary
                foregroundColor: pane.accent
                enabled: pane.editor.active && !pane.editor.stateBusy
                toolTipText: qsTr("Add an adjustment or open a structural tool")
                accessibleName: qsTr("Add node")
                onClicked: gradeNodeMenus.openAdd(addGradeNodeButton)
            }
        }

        StructuralNodeRow {
            objectName: "canvasNodeRow"
            nodeLabel: qsTr("Crop & Geometry")
            nodeStatus: qsTr("CANVAS · FIXED")
            nodeGlyph: "C"
            nodeSelected: pane.editor.selectedRecipeNodeKind === "canvas"
            onActivated: {
                pane.editor.selectCanvasNode()
                pane.cropToolRequested()
            }
        }

        StructuralNodeRow {
            objectName: "liquifyNodeRow"
            visible: pane.editor.liquifyNodeMaterialized
                || pane.editor.selectedRecipeNodeKind === "liquify"
            nodeLabel: qsTr("Liquify")
            nodeStatus: pane.editor.liquifyNodeMaterialized
                ? qsTr("PHOTO · SINGLETON")
                : qsTr("EMPTY · DRAW TO CREATE")
            nodeGlyph: "L"
            nodeSelected: pane.editor.selectedRecipeNodeKind === "liquify"
            onActivated: {
                pane.editor.selectLiquifyNode()
                pane.liquifyToolRequested()
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

                readonly property bool selected:
                    pane.editor.selectedRecipeNodeKind === "grade"
                    && gradeNodeRow.index === pane.editor.selectedGradeNodeIndex

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

        Rectangle {
            id: foundationNodeRow
            objectName: "foundationNodeRow"
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            radius: 6
            color: pane.editor.foundationSelected
                ? Theme.accentSurfaceQuiet : Theme.panelRaised
            border.width: pane.editor.foundationSelected ? 1 : 0
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
                    color: pane.editor.foundationSelected
                        ? Theme.accentSurface : Theme.surfaceSubtle
                    border.width: pane.editor.foundationSelected ? 1 : 0
                    border.color: Theme.accentBorder

                    Label {
                        anchors.centerIn: parent
                        text: "F"
                        color: pane.editor.foundationSelected
                            ? pane.accent : pane.textMuted
                        font.pixelSize: 9
                        font.weight: Font.Bold
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Basic Adjustments")
                        color: pane.editor.foundationEnabled
                            ? pane.textPrimary : pane.textSecondary
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: pane.editor.foundationEnabled
                            ? qsTr("SOURCE · ENABLED")
                            : qsTr("SOURCE · BYPASSED")
                        color: pane.editor.foundationEnabled
                            ? (pane.editor.foundationSelected
                                ? Theme.accentTextMuted : pane.textMuted)
                            : Theme.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.2
                        elide: Text.ElideRight
                    }
                }

                Switch {
                    id: foundationEnabledSwitch
                    objectName: "foundationEnabledSwitch"
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 22
                    checked: pane.editor.foundationEnabled
                    enabled: pane.editor.active && !pane.editor.stateBusy
                    Accessible.name: checked
                        ? qsTr("Bypass Basic Adjustments")
                        : qsTr("Enable Basic Adjustments")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: checked
                        ? qsTr("Bypass optional source adjustments; preserve their values")
                        : qsTr("Enable Basic Adjustments")
                    onClicked: {
                        pane.editor.selectFoundationNode()
                        pane.editor.foundationEnabled = checked
                    }

                    indicator: Rectangle {
                        implicitWidth: 34
                        implicitHeight: 18
                        x: (foundationEnabledSwitch.width - width) / 2
                        y: (foundationEnabledSwitch.height - height) / 2
                        radius: height / 2
                        color: foundationEnabledSwitch.checked
                            ? Theme.switchOnSurface : Theme.switchOffSurface
                        border.color: foundationEnabledSwitch.checked
                            ? Theme.switchOnBorder : Theme.switchOffBorder

                        Rectangle {
                            width: 12
                            height: 12
                            y: 3
                            x: foundationEnabledSwitch.checked
                                ? parent.width - width - 3 : 3
                            radius: width / 2
                            color: foundationEnabledSwitch.checked
                                ? pane.accent : pane.textMuted
                        }
                    }

                    contentItem: Item {}
                }
            }

            MouseArea {
                anchors.fill: parent
                anchors.rightMargin: 44
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.PointingHandCursor
                onClicked: pane.editor.selectFoundationNode()
            }
        }

        Rectangle {
            id: rawDenoiseNodeRow
            objectName: "rawDenoiseNodeRow"
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            radius: 6
            color: pane.editor.rawDenoiseSelected
                ? Theme.accentSurfaceQuiet : Theme.panelRaised
            border.width: pane.editor.rawDenoiseSelected ? 1 : 0
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
                    color: pane.editor.rawDenoiseSelected
                        ? Theme.accentSurface : Theme.surfaceSubtle
                    border.width: pane.editor.rawDenoiseSelected ? 1 : 0
                    border.color: Theme.accentBorder

                    Label {
                        anchors.centerIn: parent
                        text: "AI"
                        color: pane.editor.rawDenoiseSelected
                            ? pane.accent : pane.textMuted
                        font.pixelSize: 8
                        font.weight: Font.Bold
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("AI RAW Denoise")
                        color: pane.editor.foundationAiDenoiseEnabled
                            ? pane.textPrimary : pane.textSecondary
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: pane.editor.foundationAiDenoiseBusy
                            ? qsTr("PROCESSING")
                            : (pane.editor.foundationAiDenoiseEnabled
                                ? qsTr("SOURCE · %L1%").arg(
                                    pane.editor.foundationAiDenoiseAmount)
                                : qsTr("SOURCE · BYPASSED"))
                        color: pane.editor.foundationAiDenoiseEnabled
                            ? (pane.editor.rawDenoiseSelected
                                ? Theme.accentTextMuted : pane.textMuted)
                            : Theme.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.2
                        elide: Text.ElideRight
                    }
                }

                Switch {
                    id: rawDenoiseEnabledSwitch
                    objectName: "rawDenoiseEnabledSwitch"
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 22
                    checked: pane.editor.foundationAiDenoiseEnabled
                    enabled: pane.editor.active
                        && !pane.editor.stateBusy
                        && !pane.editor.foundationAiDenoiseBusy
                        && (checked || pane.editor.foundationAiDenoiseCanStart)
                    Accessible.name: checked
                        ? qsTr("Bypass AI RAW Denoise")
                        : qsTr("Enable AI RAW Denoise")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: checked
                        ? qsTr("Bypass AI RAW Denoise; preserve its cache")
                        : qsTr("Apply AI RAW Denoise")
                    onClicked: {
                        pane.editor.selectRawDenoiseNode()
                        pane.editor.foundationAiDenoiseEnabled = checked
                    }

                    indicator: Rectangle {
                        implicitWidth: 34
                        implicitHeight: 18
                        x: (rawDenoiseEnabledSwitch.width - width) / 2
                        y: (rawDenoiseEnabledSwitch.height - height) / 2
                        radius: height / 2
                        color: rawDenoiseEnabledSwitch.checked
                            ? Theme.switchOnSurface : Theme.switchOffSurface
                        border.color: rawDenoiseEnabledSwitch.checked
                            ? Theme.switchOnBorder : Theme.switchOffBorder

                        Rectangle {
                            width: 12
                            height: 12
                            y: 3
                            x: rawDenoiseEnabledSwitch.checked
                                ? parent.width - width - 3 : 3
                            radius: width / 2
                            color: rawDenoiseEnabledSwitch.checked
                                ? pane.accent : pane.textMuted
                        }
                    }

                    contentItem: Item {}
                }
            }

            MouseArea {
                anchors.fill: parent
                anchors.rightMargin: 44
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.PointingHandCursor
                onClicked: pane.editor.selectRawDenoiseNode()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: pane.editor.selectedRecipeNodeKind === "grade"
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
            text: qsTr("Read the stack from bottom source to top output. Basic Adjustments and Canvas are fixed structural nodes.")
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
