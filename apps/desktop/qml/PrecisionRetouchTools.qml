pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Photo-local deterministic repair. Heal reconstructs from a surrounding
// ring; Clone copies a same-shaped nearby source. The canvas owns direct
// manipulation while the inspector exposes compact, precise controls.
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
        toolTipText: qsTr("Remove small distractions with a feathered heal or a nearby clone source.")
        sectionEnabled: retouch.inspector.editor.active
            && !retouch.inspector.editor.stateBusy

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                text: qsTr("Heal")
                selected: retouch.inspector.editor.retouchCreationMode === 0
                toolTipText: qsTr("Blend a defect from its surrounding pixels")
                onClicked: {
                    retouch.inspector.editor.setRetouchCreationMode(0)
                    retouch.inspector.editor.setRetouchPickerActive(true)
                }
            }

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                text: qsTr("Clone")
                selected: retouch.inspector.editor.retouchCreationMode === 1
                toolTipText: qsTr("Copy a same-shaped nearby source")
                onClicked: {
                    retouch.inspector.editor.setRetouchCreationMode(1)
                    retouch.inspector.editor.setRetouchPickerActive(true)
                }
            }

            ShadowIconButton {
                source: "qrc:/icons/retouch.svg"
                selected: retouch.inspector.editor.retouchPickerActive
                toolTipText: retouch.inspector.editor.retouchPickerActive
                    ? qsTr("Stop adding repair spots")
                    : qsTr("Add repair spots")
                accessibleName: toolTipText
                onClicked: retouch.inspector.editor.setRetouchPickerActive(
                    !retouch.inspector.editor.retouchPickerActive)
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            text: retouch.inspector.editor.retouchPickerActive
                ? qsTr("Click the image to add. Drag circles to refine.")
                : qsTr("Select a circle on the image to refine it.")
            color: retouch.inspector.editor.retouchPickerActive
                ? Theme.accentTextMuted : Theme.textMuted
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: retouch.inspector.editor.retouchSpots

            delegate: ColumnLayout {
                required property var modelData

                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                spacing: 3

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Spot %1").arg(modelData.index + 1)
                        color: Theme.textSecondary
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                    }

                    ShadowButton {
                        compact: true
                        minimumButtonWidth: 42
                        text: qsTr("Heal")
                        selected: Number(modelData.mode) === 0
                        onClicked: retouch.inspector.editor.setRetouchSpotMode(
                            modelData.index, 0)
                    }

                    ShadowButton {
                        compact: true
                        minimumButtonWidth: 45
                        text: qsTr("Clone")
                        selected: Number(modelData.mode) === 1
                        onClicked: retouch.inspector.editor.setRetouchSpotMode(
                            modelData.index, 1)
                    }

                    ShadowIconButton {
                        buttonSize: 24
                        iconSize: 15
                        source: "qrc:/icons/trash.svg"
                        toolTipText: qsTr("Remove spot %1").arg(
                            modelData.index + 1)
                        accessibleName: toolTipText
                        onClicked: retouch.inspector.editor.removeRetouchSpot(
                            modelData.index)
                    }
                }

                ShadowSlider {
                    Layout.fillWidth: true
                    label: qsTr("Size")
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

                ShadowSlider {
                    Layout.fillWidth: true
                    label: qsTr("Feather")
                    from: 0
                    to: 1
                    neutralValue: 0.28
                    stepSize: 0.01
                    decimals: 0
                    displayMultiplier: 100
                    suffix: "%"
                    value: Number(modelData.feather)
                    toolTipText: qsTr("Soften the repair edge")
                    onGestureStarted: retouch.inspector.editor.beginParameterEdit(
                        "retouch/" + modelData.index + "/feather")
                    onEdited: value => retouch.inspector.editor.setRetouchSpotFeather(
                        modelData.index, value)
                    onGestureFinished: retouch.inspector.editor.endParameterEdit(
                        "retouch/" + modelData.index + "/feather")
                }

                Label {
                    Layout.fillWidth: true
                    visible: Number(modelData.mode) === 1
                    text: qsTr("Drag the linked source circle on the image.")
                    color: Theme.textMuted
                    font.pixelSize: 9
                }
            }
        }
    }
}
