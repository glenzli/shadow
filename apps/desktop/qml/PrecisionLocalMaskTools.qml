pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One node-bound mask owns an ordered component vector. This surface selects
// and edits one leaf while the renderer supplies either that leaf's exact
// coverage or the final composed coverage.
ColumnLayout {
    id: localMask

    required property var inspector
    required property int currentTabIndex

    signal createMaskRequested(var anchorItem)

    readonly property var mask: inspector.editor.selectedLocalMask
    readonly property int kind: Number(mask.kind || 0)
    readonly property bool activeMask: kind >= 1 && kind <= 6
    readonly property bool nodeEditable: inspector.editor.active && inspector.editor.hasSelectedGradeNode && inspector.editor.gradeNodeEnabled && !inspector.editor.stateBusy
    readonly property string kindLabel: kind === 1 ? qsTr("Linear gradient") : kind === 2 ? qsTr("Radial gradient") : kind === 3 ? qsTr("Brush") : kind === 4 ? qsTr("Luminance range") : kind === 5 ? qsTr("Color range") : kind === 6 ? qsTr("AI mask") : qsTr("No mask")
    readonly property url kindIcon: kind === 1 ? "qrc:/icons/mask-linear.svg" : kind === 2 ? "qrc:/icons/mask-radial.svg" : kind === 3 ? "qrc:/icons/brush.svg" : kind === 4 ? "qrc:/icons/mask-luminance-range.svg" : kind === 5 ? "qrc:/icons/mask-color-range.svg" : kind === 6 ? "qrc:/icons/mask.svg" : "qrc:/icons/mask-create.svg"

    function operationLabel(operation) {
        return operation === 0 ? qsTr("Base")
            : operation === 1 ? qsTr("Add")
            : operation === 2 ? qsTr("Subtract") : qsTr("Intersect")
    }

    function componentKindLabel(kindValue) {
        return kindValue === 1 ? qsTr("Linear gradient")
            : kindValue === 2 ? qsTr("Radial gradient")
            : kindValue === 3 ? qsTr("Brush")
            : kindValue === 4 ? qsTr("Luminance range")
            : kindValue === 5 ? qsTr("Color range") : qsTr("AI mask")
    }

    spacing: 8

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: localMask.currentTabIndex === 0 && localMask.inspector.editor.aiMaskPromptActive
        title: localMask.inspector.editor.aiMaskFaceRegionMode
            ? qsTr("PEOPLE DETAIL MASK")
            : localMask.inspector.editor.aiMaskSemanticMode
                ? qsTr("SEMANTIC MASK") : qsTr("AI SUBJECT MASK")
        summary: localMask.inspector.editor.aiMaskBusy
            ? localMask.inspector.editor.aiMaskSemanticMode
                ? qsTr("Locating semantic subject…")
            : localMask.inspector.editor.aiMaskFaceRegionMode
                ? qsTr("Identifying facial details…")
                : qsTr("Identifying subject…")
            : localMask.inspector.editor.aiMaskHasCandidate
                ? qsTr("Selection preview")
                : localMask.inspector.editor.aiMaskSemanticMode
                    ? qsTr("Semantic selection needs retry")
                : localMask.inspector.editor.aiMaskFaceRegionMode
                    ? localMask.inspector.editor.aiMaskPeople.length > 0
                        ? qsTr("Choose a person and details")
                        : qsTr("Detecting people")
                    : localMask.inspector.editor.aiMaskPromptPoints.length > 0
                        ? qsTr("Selection needs retry")
                        : qsTr("Click the object")
        toolTipText: localMask.inspector.editor.aiMaskFaceRegionMode
            ? qsTr("Choose one person and combine visible details. The result remains the current Grade Node's mask.")
            : localMask.inspector.editor.aiMaskSemanticMode
                ? qsTr("The accepted pixels stay with this photo; the semantic instruction is re-evaluated when copied.")
            : qsTr("Include points identify the subject. Exclude points remove nearby regions.")
        sectionEnabled: true
        resetAvailable: true
        resetEnabled: !localMask.inspector.editor.aiMaskBusy
            && (localMask.inspector.editor.aiMaskPromptPoints.length > 0
                || localMask.inspector.editor.aiMaskHasCandidate)
        onResetRequested: {
            if (!localMask.inspector.editor.aiMaskFaceRegionMode
                    && !localMask.inspector.editor.aiMaskSemanticMode)
                localMask.inspector.editor.aiMaskForegroundMode = true
            localMask.inspector.editor.clearAiMaskPromptPoints()
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            font.pixelSize: Theme.fontMeta
            text: localMask.inspector.editor.aiMaskFaceRegionMode
                ? qsTr("People are detected automatically. Choose one person, then select one or more visible details to preview their combined mask.")
                : localMask.inspector.editor.aiMaskSemanticMode
                    ? qsTr("Looking for “%1”. Preview the result before applying it; no photo pixels are copied from another image.").arg(localMask.inspector.editor.aiMaskSemanticQuery)
                : qsTr("Click the object to create a selection. Add or subtract points to refine the visible overlay.")
        }

        PrecisionPeopleMaskSelector {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: localMask.inspector.editor.aiMaskFaceRegionMode
            editor: localMask.inspector.editor
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 8
            visible: !localMask.inspector.editor.aiMaskFaceRegionMode
                && !localMask.inspector.editor.aiMaskSemanticMode

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Add selection (+)")
                variant: localMask.inspector.editor.aiMaskForegroundMode ? ShadowButton.Primary : ShadowButton.Ghost
                enabled: !localMask.inspector.editor.aiMaskBusy
                onClicked: localMask.inspector.editor.aiMaskForegroundMode = true
            }

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Subtract selection (−)")
                variant: !localMask.inspector.editor.aiMaskForegroundMode ? ShadowButton.Primary : ShadowButton.Ghost
                enabled: !localMask.inspector.editor.aiMaskBusy
                onClicked: localMask.inspector.editor.aiMaskForegroundMode = false
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 8
            visible: !localMask.inspector.editor.aiMaskFaceRegionMode
                && !localMask.inspector.editor.aiMaskSemanticMode

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Undo point")
                variant: ShadowButton.Ghost
                enabled: !localMask.inspector.editor.aiMaskBusy && localMask.inspector.editor.aiMaskPromptPoints.length > 0
                onClicked: localMask.inspector.editor.undoAiMaskPromptPoint()
            }

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Clear")
                variant: ShadowButton.Ghost
                enabled: !localMask.inspector.editor.aiMaskBusy && localMask.inspector.editor.aiMaskPromptPoints.length > 0
                onClicked: localMask.inspector.editor.clearAiMaskPromptPoints()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 8

            ShadowButton {
                Layout.fillWidth: true
                visible: !localMask.inspector.editor.aiMaskBusy
                    && !localMask.inspector.editor.aiMaskHasCandidate
                    && (localMask.inspector.editor.aiMaskPromptPoints.length > 0
                        || localMask.inspector.editor.aiMaskSemanticMode)
                text: qsTr("Retry selection")
                variant: ShadowButton.Secondary
                enabled: localMask.inspector.editor.aiMaskCanGenerate && !localMask.inspector.editor.aiMaskBusy
                onClicked: localMask.inspector.editor.generateAiMask()
            }

            ShadowButton {
                text: qsTr("Cancel")
                variant: ShadowButton.Ghost
                onClicked: localMask.inspector.editor.cancelAiMaskPrompt()
            }
        }

        ShadowButton {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: localMask.inspector.editor.aiMaskHasCandidate
            text: qsTr("Apply mask")
            variant: ShadowButton.Primary
            enabled: !localMask.inspector.editor.aiMaskBusy
            onClicked: localMask.inspector.editor.applyAiMaskCandidate()
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: localMask.currentTabIndex === 0 && !localMask.inspector.editor.aiMaskPromptActive
        title: qsTr("NODE MASK")
        summary: localMask.kindLabel
        toolTipText: qsTr("Edit the selector attached to this Grade Node.")
        sectionEnabled: localMask.nodeEditable
        resetAvailable: true
        resetEnabled: localMask.activeMask
        onResetRequested:
            localMask.inspector.editor.resetSelectedLocalMask()

        ColumnLayout {
            objectName: "maskComponentList"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 4
            visible: localMask.inspector.editor.localMaskComponents.length > 0

            Repeater {
                model: localMask.inspector.editor.localMaskComponents

                delegate: Rectangle {
                    id: componentRow
                    required property int index
                    required property var modelData

                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    radius: 6
                    color: componentRow.modelData.selected
                        ? Theme.accentSurfaceQuiet : Theme.surfaceSubtle
                    border.width: componentRow.modelData.selected ? 1 : 0
                    border.color: Theme.accentBorder

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 9
                        anchors.rightMargin: 8
                        spacing: 7

                        Label {
                            text: localMask.operationLabel(componentRow.modelData.operation)
                            color: componentRow.modelData.enabled
                                ? Theme.textSecondary : Theme.textDisabled
                            font.pixelSize: Theme.fontCaption
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: localMask.componentKindLabel(componentRow.modelData.kind)
                            color: componentRow.modelData.enabled
                                ? Theme.textPrimary : Theme.textDisabled
                            font.pixelSize: Theme.fontMeta
                            elide: Text.ElideRight
                        }
                        ShadowSwitch {
                            compact: true
                            checked: Boolean(componentRow.modelData.enabled)
                            enabled: localMask.nodeEditable
                            Accessible.name: qsTr("Enable mask component")
                            onClicked: {
                                localMask.inspector.editor.selectLocalMaskComponent(componentRow.index)
                                localMask.inspector.editor.setSelectedLocalMaskComponentEnabled(checked)
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        anchors.rightMargin: 42
                        cursorShape: Qt.PointingHandCursor
                        onClicked: localMask.inspector.editor.selectLocalMaskComponent(componentRow.index)
                    }
                }
            }
        }

        RowLayout {
            objectName: "maskCoverageModeSelector"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: localMask.activeMask
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: qsTr("Overlay")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
            }
            ShadowButton {
                compact: true
                text: qsTr("Selected")
                variant: localMask.inspector.editor.maskCoverageShowsSelectedComponent
                    ? ShadowButton.Primary : ShadowButton.Ghost
                onClicked: localMask.inspector.editor.maskCoverageShowsSelectedComponent = true
            }
            ShadowButton {
                compact: true
                text: qsTr("Combined")
                variant: !localMask.inspector.editor.maskCoverageShowsSelectedComponent
                    ? ShadowButton.Primary : ShadowButton.Ghost
                onClicked: localMask.inspector.editor.maskCoverageShowsSelectedComponent = false
            }
        }

        RowLayout {
            objectName: "selectedMaskComponentOperationSelector"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: localMask.activeMask
                && Number(localMask.mask.componentIndex || 0) > 0
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: qsTr("Combine")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
            }
            ShadowButton {
                compact: true
                text: qsTr("Add")
                variant: Number(localMask.mask.operation) === 1
                    ? ShadowButton.Primary : ShadowButton.Ghost
                onClicked: localMask.inspector.editor.setSelectedLocalMaskComponentOperation(1)
            }
            ShadowButton {
                compact: true
                text: qsTr("Subtract")
                variant: Number(localMask.mask.operation) === 2
                    ? ShadowButton.Primary : ShadowButton.Ghost
                onClicked: localMask.inspector.editor.setSelectedLocalMaskComponentOperation(2)
            }
            ShadowButton {
                compact: true
                text: qsTr("Intersect")
                variant: Number(localMask.mask.operation) === 3
                    ? ShadowButton.Primary : ShadowButton.Ghost
                onClicked: localMask.inspector.editor.setSelectedLocalMaskComponentOperation(3)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 8

            Rectangle {
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                radius: 7
                color: Theme.accentSurfaceQuiet

                ShadowIcon {
                    anchors.centerIn: parent
                    source: localMask.kindIcon
                    color: localMask.inspector.accent
                    size: 20
                }
            }

            Label {
                Layout.fillWidth: true
                text: localMask.kindLabel
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
            }

            ShadowIconButton {
                visible: localMask.kind === 3
                buttonSize: 36
                iconSize: 19
                source: "qrc:/icons/eraser.svg"
                toolTipText: qsTr("Clear brush strokes")
                accessibleName: toolTipText
                enabled: localMask.nodeEditable && (localMask.mask.brushPoints || []).length > 0
                onClicked: localMask.inspector.editor.clearSelectedLocalMaskBrush()
            }

            ShadowIconButton {
                visible: localMask.activeMask
                buttonSize: 36
                iconSize: 19
                source: "qrc:/icons/trash.svg"
                variant: ShadowIconButton.Danger
                toolTipText: Number(localMask.mask.componentCount || 0) > 1
                    ? qsTr("Remove selected mask component") : qsTr("Remove this node mask")
                accessibleName: toolTipText
                enabled: localMask.nodeEditable
                onClicked: localMask.inspector.editor.removeSelectedLocalMaskComponent()
            }
        }

        ShadowButton {
            id: createMaskButton

            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: !localMask.activeMask
            text: qsTr("Create mask")
            variant: ShadowButton.Secondary
            enabled: localMask.nodeEditable
            onClicked: localMask.createMaskRequested(createMaskButton)
        }

        ShadowButton {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: !localMask.activeMask && localMask.inspector.editor.hasCopiedNodeMask
            text: qsTr("Paste as mask")
            variant: ShadowButton.Ghost
            enabled: localMask.nodeEditable
            toolTipText: qsTr("Attach the copied mask geometry to this node")
            onClicked: localMask.inspector.editor.pasteSelectedLocalMask()
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: localMask.activeMask && localMask.kind !== 6
            spacing: 8

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Copy")
                toolTipText: qsTr("Copy this mask geometry")
                enabled: localMask.nodeEditable
                onClicked: localMask.inspector.editor.copySelectedLocalMask()
            }

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Replace from clipboard")
                toolTipText: qsTr("Replace this mask with copied geometry · Undo available")
                enabled: localMask.nodeEditable && localMask.inspector.editor.hasCopiedNodeMask
                onClicked: localMask.inspector.editor.pasteSelectedLocalMask()
            }
        }

        Repeater {
            model: localMask.kind === 1 ? [
                {
                    "key": "x0",
                    "name": qsTr("Start X")
                },
                {
                    "key": "y0",
                    "name": qsTr("Start Y")
                },
                {
                    "key": "x1",
                    "name": qsTr("End X")
                },
                {
                    "key": "y1",
                    "name": qsTr("End Y")
                }
            ] : localMask.kind === 2 ? [
                {
                    "key": "x0",
                    "name": qsTr("Center X")
                },
                {
                    "key": "y0",
                    "name": qsTr("Center Y")
                },
                {
                    "key": "radiusX",
                    "name": qsTr("Width"),
                    "from": 0.01
                },
                {
                    "key": "radiusY",
                    "name": qsTr("Height"),
                    "from": 0.01
                },
                {
                    "key": "feather",
                    "name": qsTr("Feather")
                }
            ] : localMask.kind === 3 ? [
                {
                    "key": "radiusX",
                    "name": qsTr("Size"),
                    "from": 0.005,
                    "neutral": 0.035
                },
                {
                    "key": "feather",
                    "name": qsTr("Feather"),
                    "neutral": 0.6
                }
            ] : localMask.kind === 4 ? [
                {
                    "key": "lower",
                    "name": qsTr("Lower"),
                    "to": Number(localMask.mask.upper),
                    "neutral": 0.2
                },
                {
                    "key": "upper",
                    "name": qsTr("Upper"),
                    "from": Number(localMask.mask.lower),
                    "neutral": 0.8
                },
                {
                    "key": "softness",
                    "name": qsTr("Softness"),
                    "neutral": 0.08
                }
            ] : localMask.kind === 5 ? [
                {
                    "key": "centerHue",
                    "name": qsTr("Hue"),
                    "from": 0,
                    "to": 359 / 360,
                    "neutral": 30 / 360,
                    "step": 1 / 360,
                    "multiplier": 360,
                    "suffix": "°"
                },
                {
                    "key": "width",
                    "name": qsTr("Range"),
                    "from": 1 / 180,
                    "neutral": 30 / 180,
                    "step": 1 / 180,
                    "multiplier": 180,
                    "suffix": "°"
                },
                {
                    "key": "softness",
                    "name": qsTr("Softness"),
                    "neutral": 0.45
                }
            ] : localMask.kind === 6 ? [
                {
                    "key": "x0",
                    "name": qsTr("Expand / Contract"),
                    "from": -1,
                    "to": 1,
                    "neutral": 0
                },
                {
                    "key": "feather",
                    "name": qsTr("Feather"),
                    "neutral": 0
                }
            ] : []

            delegate: ShadowSlider {
                required property var modelData

                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                label: modelData.name
                from: modelData.from === undefined ? 0 : modelData.from
                to: modelData.to === undefined ? 1 : modelData.to
                neutralValue: modelData.neutral === undefined ? 0.5 : modelData.neutral
                stepSize: modelData.step === undefined ? (modelData.key === "radiusX" && localMask.kind === 3 ? 0.005 : 0.01) : modelData.step
                decimals: 0
                displayMultiplier: modelData.multiplier === undefined ? 100 : modelData.multiplier
                suffix: modelData.suffix === undefined ? "%" : modelData.suffix
                value: Number(localMask.mask[modelData.key] || 0)
                enabled: localMask.nodeEditable
                onGestureStarted: localMask.inspector.editor.beginParameterEdit("local_mask/" + modelData.key)
                onEdited: value => localMask.inspector.editor.setSelectedLocalMaskValue(modelData.key, value)
                onGestureFinished: localMask.inspector.editor.endParameterEdit("local_mask/" + modelData.key)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: localMask.activeMask
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Invert selected component")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
            }

            ShadowSwitch {
                id: leafInvertSwitch

                Layout.preferredWidth: 36
                Layout.preferredHeight: 22
                compact: true
                accentColor: localMask.inspector.accent
                checked: Boolean(localMask.mask.leafInverted)
                enabled: localMask.nodeEditable
                Accessible.name: qsTr("Invert selected mask component")
                onClicked: localMask.inspector.editor.setSelectedLocalMaskLeafInverted(checked)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: localMask.activeMask
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Invert combined result")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
            }

            ShadowSwitch {
                id: finalInvertSwitch

                Layout.preferredWidth: 36
                Layout.preferredHeight: 22
                compact: true
                accentColor: localMask.inspector.accent
                checked: Boolean(localMask.mask.finalInverted)
                enabled: localMask.nodeEditable
                Accessible.name: qsTr("Invert combined node mask")
                onClicked: localMask.inspector.editor.setSelectedLocalMaskInverted(checked)
            }
        }
    }
}
