pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The tool edits the selected node's one current selector. Creation and node
// assignment live in PrecisionMaskCreateMenu; this surface owns only mask
// geometry and in-session copy/paste.
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

    spacing: 8

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: localMask.currentTabIndex === 0 && localMask.inspector.editor.aiMaskPromptActive
        title: qsTr("AI MASK")
        summary: localMask.inspector.editor.aiMaskBusy
            ? qsTr("Identifying subject…")
            : localMask.inspector.editor.aiMaskHasCandidate
                ? qsTr("Candidate ready")
                : qsTr("%1 prompt points").arg(localMask.inspector.editor.aiMaskPromptPoints.length)
        toolTipText: qsTr("Include points identify the subject. Exclude points remove nearby regions.")
        sectionEnabled: true
        resetAvailable: true
        resetEnabled: !localMask.inspector.editor.aiMaskBusy
            && (localMask.inspector.editor.aiMaskPromptPoints.length > 0
                || localMask.inspector.editor.aiMaskHasCandidate)
        onResetRequested: {
            localMask.inspector.editor.aiMaskForegroundMode = true
            localMask.inspector.editor.clearAiMaskPromptPoints()
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 8

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Include")
                variant: localMask.inspector.editor.aiMaskForegroundMode ? ShadowButton.Secondary : ShadowButton.Ghost
                enabled: !localMask.inspector.editor.aiMaskBusy
                onClicked: localMask.inspector.editor.aiMaskForegroundMode = true
            }

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Exclude")
                variant: !localMask.inspector.editor.aiMaskForegroundMode ? ShadowButton.Secondary : ShadowButton.Ghost
                enabled: !localMask.inspector.editor.aiMaskBusy
                onClicked: localMask.inspector.editor.aiMaskForegroundMode = false
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 8

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
                text: localMask.inspector.editor.aiMaskBusy
                    ? qsTr("Generating…")
                    : localMask.inspector.editor.aiMaskHasCandidate
                        ? qsTr("Regenerate")
                        : qsTr("Generate")
                variant: localMask.inspector.editor.aiMaskHasCandidate
                    ? ShadowButton.Secondary : ShadowButton.Primary
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
            text: qsTr("Apply candidate")
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
                font.pixelSize: 11
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
                toolTipText: qsTr("Remove this node mask")
                accessibleName: toolTipText
                enabled: localMask.nodeEditable
                onClicked: localMask.inspector.editor.setSelectedLocalMask(0)
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
                text: qsTr("Invert")
                color: Theme.textSecondary
                font.pixelSize: 10
            }

            Switch {
                id: invertSwitch

                Layout.preferredWidth: 36
                Layout.preferredHeight: 22
                checked: Boolean(localMask.mask.inverted)
                enabled: localMask.nodeEditable
                Accessible.name: qsTr("Invert node mask")
                onClicked: localMask.inspector.editor.setSelectedLocalMaskInverted(checked)

                indicator: Rectangle {
                    implicitWidth: 34
                    implicitHeight: 18
                    x: (invertSwitch.width - width) / 2
                    y: (invertSwitch.height - height) / 2
                    radius: height / 2
                    color: invertSwitch.checked ? Theme.switchOnSurface : Theme.switchOffSurface
                    border.color: invertSwitch.checked ? Theme.switchOnBorder : Theme.switchOffBorder

                    Rectangle {
                        width: 12
                        height: 12
                        y: 3
                        x: invertSwitch.checked ? parent.width - width - 3 : 3
                        radius: width / 2
                        color: invertSwitch.checked ? localMask.inspector.accent : Theme.textMuted
                    }
                }
                contentItem: Item {}
            }
        }
    }
}
