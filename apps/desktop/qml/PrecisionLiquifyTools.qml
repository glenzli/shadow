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

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                variant: ShadowButton.Ghost
                text: qsTr("Clear")
                enabled: liquify.inspector.editor.liquifyStrokes.length > 0
                toolTipText: qsTr("Remove the complete Liquify node")
                onClicked: liquify.inspector.editor.clearLiquify()
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 8
            Layout.bottomMargin: 10
            text: qsTr("Drag on the image to push pixels. The path stays local while dragging, then commits as one undoable stroke.")
            color: Theme.textMuted
            font.pixelSize: 10
            wrapMode: Text.WordWrap
            lineHeight: 1.25
        }
    }
}
