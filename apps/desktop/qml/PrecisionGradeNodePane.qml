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
        property bool bypassAvailable: false
        property bool nodeEnabled: true

        signal activated
        signal enabledToggled(bool enabled)

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

            ShadowIconButton {
                id: structuralVisibilityButton
                visible: structuralRow.bypassAvailable
                Layout.preferredWidth: visible ? 28 : 0
                buttonSize: 28
                iconSize: 16
                variant: ShadowIconButton.Ghost
                source: structuralRow.nodeEnabled
                    ? "qrc:/icons/overlay-show.svg"
                    : "qrc:/icons/overlay-hide.svg"
                foregroundColor: structuralRow.nodeEnabled
                    ? pane.textSecondary : pane.textMuted
                enabled: pane.editor.active && !pane.editor.stateBusy
                toolTipText: structuralRow.nodeEnabled
                    ? qsTr("Hide %1").arg(structuralRow.nodeLabel)
                    : qsTr("Show %1").arg(structuralRow.nodeLabel)
                accessibleName: toolTipText
                Accessible.checked: structuralRow.nodeEnabled
                onClicked: structuralRow.enabledToggled(!structuralRow.nodeEnabled)
            }
        }

        MouseArea {
            anchors.fill: parent
            anchors.rightMargin: structuralRow.bypassAvailable ? 44 : 0
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
                source: "qrc:/icons/grade-node-add.svg"
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
            visible: pane.editor.canvasNodeMaterialized
            nodeLabel: qsTr("Crop & Geometry")
            nodeStatus: pane.editor.canvasNodeEnabled
                ? qsTr("PHOTO · ENABLED") : qsTr("PHOTO · BYPASSED")
            nodeGlyph: "C"
            nodeSelected: pane.editor.selectedRecipeNodeKind === "canvas"
            bypassAvailable: true
            nodeEnabled: pane.editor.canvasNodeEnabled
            onEnabledToggled: enabled => pane.editor.canvasNodeEnabled = enabled
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
                ? (pane.editor.liquifyNodeEnabled
                    ? qsTr("PHOTO · ENABLED")
                    : qsTr("PHOTO · BYPASSED"))
                : qsTr("EMPTY · DRAW TO CREATE")
            nodeGlyph: "L"
            nodeSelected: pane.editor.selectedRecipeNodeKind === "liquify"
            bypassAvailable: pane.editor.liquifyNodeMaterialized
            nodeEnabled: pane.editor.liquifyNodeEnabled
            onEnabledToggled: enabled =>
                pane.editor.liquifyNodeEnabled = enabled
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
                        ShadowIcon {
                            anchors.centerIn: parent
                            source: "qrc:/icons/grade-node.svg"
                            color: gradeNodeRow.selected ? pane.accent : pane.textMuted
                            size: 14
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

                    ShadowIconButton {
                        id: gradeNodeVisibilityButton
                        objectName: "gradeNodeVisibilityButton"
                        buttonSize: 28
                        iconSize: 16
                        variant: ShadowIconButton.Ghost
                        source: gradeNodeRow.modelData.enabled
                            ? "qrc:/icons/overlay-show.svg"
                            : "qrc:/icons/overlay-hide.svg"
                        foregroundColor: gradeNodeRow.modelData.enabled
                            ? pane.textSecondary : pane.textMuted
                        enabled: pane.editor.active && !pane.editor.stateBusy
                        toolTipText: gradeNodeRow.modelData.enabled
                            ? qsTr("Hide %1").arg(gradeNodeRow.modelData.label)
                            : qsTr("Show %1").arg(gradeNodeRow.modelData.label)
                        accessibleName: toolTipText
                        Accessible.checked: gradeNodeRow.modelData.enabled
                        onClicked: {
                            pane.editor.selectGradeNode(gradeNodeRow.index);
                            pane.editor.gradeNodeEnabled =
                                !gradeNodeRow.modelData.enabled;
                        }
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

                ShadowIconButton {
                    id: foundationVisibilityButton
                    objectName: "foundationVisibilityButton"
                    buttonSize: 28
                    iconSize: 16
                    variant: ShadowIconButton.Ghost
                    source: pane.editor.foundationEnabled
                        ? "qrc:/icons/overlay-show.svg"
                        : "qrc:/icons/overlay-hide.svg"
                    foregroundColor: pane.editor.foundationEnabled
                        ? pane.textSecondary : pane.textMuted
                    enabled: pane.editor.active && !pane.editor.stateBusy
                    toolTipText: pane.editor.foundationEnabled
                        ? qsTr("Hide Basic Adjustments")
                        : qsTr("Show Basic Adjustments")
                    accessibleName: toolTipText
                    Accessible.checked: pane.editor.foundationEnabled
                    onClicked: {
                        pane.editor.selectFoundationNode()
                        pane.editor.foundationEnabled =
                            !pane.editor.foundationEnabled
                    }
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
            visible: pane.editor.rawDenoiseNodeMaterialized
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
                        color: pane.editor.rawDenoiseNodeVisible
                            ? pane.textPrimary : pane.textSecondary
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: !pane.editor.rawDenoiseNodeVisible
                            ? qsTr("NODE · HIDDEN")
                            : (pane.editor.foundationAiDenoiseBusy
                            ? qsTr("PROCESSING")
                            : (pane.editor.foundationAiDenoiseEnabled
                                ? qsTr("SOURCE · %L1%").arg(
                                    pane.editor.foundationAiDenoiseAmount)
                                : (pane.editor.foundationAiDenoiseCanApply
                                    ? qsTr("SOURCE · AI OFF")
                                    : qsTr("SOURCE · NOT GENERATED"))))
                        color: pane.editor.rawDenoiseNodeVisible
                            ? (pane.editor.rawDenoiseSelected
                                ? Theme.accentTextMuted : pane.textMuted)
                            : Theme.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.2
                        elide: Text.ElideRight
                    }
                }

                ShadowIconButton {
                    id: rawDenoiseVisibilityButton
                    objectName: "rawDenoiseVisibilityButton"
                    buttonSize: 28
                    iconSize: 16
                    variant: ShadowIconButton.Ghost
                    source: pane.editor.rawDenoiseNodeVisible
                        ? "qrc:/icons/overlay-show.svg"
                        : "qrc:/icons/overlay-hide.svg"
                    foregroundColor: pane.editor.rawDenoiseNodeVisible
                        ? pane.textSecondary : pane.textMuted
                    enabled: pane.editor.active
                        && !pane.editor.stateBusy
                    toolTipText: pane.editor.rawDenoiseNodeVisible
                        ? qsTr("Hide AI RAW Denoise node")
                        : qsTr("Show AI RAW Denoise node")
                    accessibleName: toolTipText
                    Accessible.checked: pane.editor.rawDenoiseNodeVisible
                    onClicked: {
                        pane.editor.selectRawDenoiseNode()
                        pane.editor.rawDenoiseNodeVisible =
                            !pane.editor.rawDenoiseNodeVisible
                    }
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

        RowLayout {
            Layout.fillWidth: true
            visible: pane.editor.selectedRecipeNodeKind === "raw_denoise"
                || pane.editor.selectedRecipeNodeKind === "canvas"
            spacing: 5

            Item {
                Layout.fillWidth: true
            }

            ShadowIconButton {
                objectName: "deleteFixedPhotoNodeButton"
                source: "qrc:/icons/trash.svg"
                toolTipText: pane.editor.selectedRecipeNodeKind === "raw_denoise"
                    ? qsTr("Remove AI RAW Denoise")
                    : qsTr("Remove Crop & Geometry")
                accessibleName: toolTipText
                enabled: pane.editor.active && !pane.editor.stateBusy
                    && !pane.editor.foundationAiDenoiseBusy
                onClicked: {
                    if (pane.editor.selectedRecipeNodeKind === "raw_denoise")
                        pane.editor.removeRawDenoiseNode()
                    else
                        pane.editor.removeCanvasNode()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: pane.borderColor
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Read from bottom source to top output. Fixed-order photo nodes appear only after you add them.")
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
