pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Photo-local, deterministic repair.  The inspector only owns picking and
// radius controls; source-space reconstruction stays in the image kernel and
// is appended after the Grade Node stack by the recipe compiler.
ColumnLayout {
    id: retouch

    required property var inspector
    required property int currentTabIndex

    spacing: 0

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: retouch.currentTabIndex === 0
        title: qsTr("REPAIR")
        summary: retouch.inspector.editor.retouchSpots.length > 0
            ? qsTr("%1 spots").arg(retouch.inspector.editor.retouchSpots.length)
            : qsTr("None")
        toolTipText: qsTr("Repair small dust spots or distractions with nearby pixels. This is deterministic reconstruction, not generative fill.")
        sectionEnabled: retouch.inspector.editor.active
            && !retouch.inspector.editor.stateBusy

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: retouch.inspector.editor.retouchPickerActive
                    ? qsTr("Click a small defect in the image")
                    : qsTr("Small defects only")
                color: retouch.inspector.editor.retouchPickerActive
                    ? retouch.inspector.accent : Theme.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
            }

            ShadowIconButton {
                source: "qrc:/icons/retouch.svg"
                selected: retouch.inspector.editor.retouchPickerActive
                toolTipText: qsTr("Add repair spot")
                accessibleName: toolTipText
                onClicked: retouch.inspector.editor.setRetouchPickerActive(
                    !retouch.inspector.editor.retouchPickerActive)
            }
        }

        Label {
            visible: retouch.inspector.editor.retouchSpots.length === 0
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            text: qsTr("Use the repair picker, then adjust its full-resolution radius.")
            color: Theme.textMuted
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: retouch.inspector.editor.retouchSpots

            delegate: RowLayout {
                required property var modelData

                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                spacing: 4

                ShadowSlider {
                    Layout.fillWidth: true
                    label: qsTr("Spot %1").arg(modelData.index + 1)
                    from: 1
                    to: 128
                    neutralValue: 18
                    stepSize: 1
                    decimals: 0
                    suffix: qsTr(" px")
                    value: modelData.radius
                    toolTipText: qsTr("Full-resolution repair radius")
                    onGestureStarted: retouch.inspector.editor.beginParameterEdit(
                        "retouch/" + modelData.index + "/radius")
                    onEdited: value => retouch.inspector.editor.setRetouchSpotRadius(
                        modelData.index, Math.round(value))
                    onGestureFinished: retouch.inspector.editor.endParameterEdit(
                        "retouch/" + modelData.index + "/radius")
                }

                ShadowIconButton {
                    buttonSize: 24
                    iconSize: 15
                    source: "qrc:/icons/trash.svg"
                    toolTipText: qsTr("Remove repair spot %1").arg(modelData.index + 1)
                    accessibleName: toolTipText
                    onClicked: retouch.inspector.editor.removeRetouchSpot(modelData.index)
                }
            }
        }
    }
}
