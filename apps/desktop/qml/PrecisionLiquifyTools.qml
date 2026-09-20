pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Photo-private Liquify authoring. Brush controls are transient defaults for
// the next gesture; each completed stroke persists its own exact values in the
// singleton structural node.
ColumnLayout {
    id: liquify

    required property var inspector
    required property int currentTabIndex

    spacing: 0

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: liquify.currentTabIndex === 0
        title: qsTr("LIQUIFY")
        summary: liquify.inspector.editor.liquifyStrokes.length > 0
            ? qsTr("%1 strokes").arg(
                liquify.inspector.editor.liquifyStrokes.length)
            : qsTr("None")
        toolTipText: qsTr("Push pixels non-destructively before the final crop. Liquify stays private to this photo and cannot be shared as a Grade Node.")
        sectionEnabled: liquify.inspector.editor.active
            && !liquify.inspector.editor.stateBusy
        resetAvailable: true
        onResetRequested: {
            liquify.inspector.editor.clearLiquify()
            liquify.inspector.editor.liquifyBrushMode = 0
            liquify.inspector.editor.liquifyBrushRadius = 0.08
            liquify.inspector.editor.liquifyBrushStrength = 0.5
            liquify.inspector.editor.liquifyBrushHardness = 0.5
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 8
            spacing: 6

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                selected:
                    liquify.inspector.editor.liquifyBrushMode === 0
                    || !liquify.inspector.editor.liquifyCanReconstruct
                text: qsTr("Push")
                toolTipText: qsTr("Move pixels along the pointer path.")
                onClicked:
                    liquify.inspector.editor.liquifyBrushMode = 0
            }

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                selected:
                    liquify.inspector.editor.liquifyBrushMode === 1
                    && liquify.inspector.editor.liquifyCanReconstruct
                text: qsTr("Reconstruct")
                enabled: liquify.inspector.editor.liquifyCanReconstruct
                    && liquify.inspector.editor.liquifyNodeEnabled
                toolTipText: qsTr("Restore deformation toward the original image mapping.")
                onClicked:
                    liquify.inspector.editor.liquifyBrushMode = 1
            }
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            label: qsTr("Size")
            toolTipText: qsTr("Brush radius as a percentage of the original image's shorter edge.")
            from: 0.005
            to: 0.25
            neutralValue: 0.08
            stepSize: 0.005
            decimals: 1
            displayMultiplier: 100
            suffix: "%"
            value: liquify.inspector.editor.liquifyBrushRadius
            onEdited: value =>
                liquify.inspector.editor.liquifyBrushRadius = value
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Strength")
            toolTipText: qsTr("Controls how far pixels follow each pointer segment.")
            from: 0.01
            to: 1
            neutralValue: 0.5
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: liquify.inspector.editor.liquifyBrushStrength
            onEdited: value =>
                liquify.inspector.editor.liquifyBrushStrength = value
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Hardness")
            toolTipText: qsTr("Keeps a larger solid core before the brush falloff.")
            from: 0
            to: 1
            neutralValue: 0.5
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: liquify.inspector.editor.liquifyBrushHardness
            onEdited: value =>
                liquify.inspector.editor.liquifyBrushHardness = value
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            spacing: 8

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                variant: ShadowButton.Ghost
                text: qsTr("Undo")
                enabled: liquify.inspector.editor.canUndo
                toolTipText: qsTr("Undo the last session adjustment")
                onClicked: liquify.inspector.editor.undo()
            }

        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 8
            Layout.bottomMargin: 10
            text: liquify.inspector.editor.liquifyBrushMode === 1
                ? qsTr("Paint over a deformed area to restore its original mapping. The authoritative preview updates while you drag, then commits as one undoable stroke.")
                : qsTr("Drag on the image to push pixels. The path stays local while dragging, then commits as one undoable stroke.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
            lineHeight: 1.25
        }
    }
}
