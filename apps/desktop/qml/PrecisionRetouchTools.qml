pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Photo-local deterministic repair. Heal reconstructs from a surrounding
// ring; Clone copies a same-shaped nearby source. Painting creates an
// overlapping sequence of the existing bounded repair targets, so the
// rendered/persisted behavior remains deterministic while the UI reads as a
// continuous repair region.
ColumnLayout {
    id: retouch

    required property var inspector
    required property int currentTabIndex

    spacing: 0

    component RetouchRegionControls: ColumnLayout {
        id: regionControls
        required property var region
        required property bool continuous
        required property int displayIndex

        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        spacing: 3

        function setMode(mode) {
            if (continuous) {
                retouch.inspector.editor.setRetouchStrokeMode(
                    region.index, mode)
            } else {
                retouch.inspector.editor.setRetouchSpotMode(
                    region.index, mode)
            }
        }

        function remove() {
            if (continuous)
                retouch.inspector.editor.removeRetouchStroke(region.index)
            else
                retouch.inspector.editor.removeRetouchSpot(region.index)
        }

        function setRadius(value) {
            if (continuous) {
                retouch.inspector.editor.setRetouchStrokeRadius(
                    region.index, Math.round(value))
            } else {
                retouch.inspector.editor.setRetouchSpotRadius(
                    region.index, Math.round(value))
            }
        }

        function setFeather(value) {
            if (continuous)
                retouch.inspector.editor.setRetouchStrokeFeather(region.index, value)
            else
                retouch.inspector.editor.setRetouchSpotFeather(region.index, value)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: qsTr("Region %1").arg(displayIndex + 1)
                color: Theme.textSecondary
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }

            ShadowButton {
                compact: true
                minimumButtonWidth: 42
                text: qsTr("Heal")
                selected: Number(region.mode) === 0
                onClicked: regionControls.setMode(0)
            }

            ShadowButton {
                compact: true
                minimumButtonWidth: 45
                text: qsTr("Clone")
                selected: Number(region.mode) === 1
                onClicked: regionControls.setMode(1)
            }

            ShadowIconButton {
                buttonSize: 24
                iconSize: 15
                source: "qrc:/icons/trash.svg"
                toolTipText: qsTr("Remove region %1").arg(displayIndex + 1)
                accessibleName: toolTipText
                onClicked: regionControls.remove()
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
            value: region.radius
            toolTipText: qsTr("Full-resolution repair radius")
            onGestureStarted: retouch.inspector.editor.beginParameterEdit(
                continuous
                    ? "retouch/stroke/" + region.index + "/radius"
                    : "retouch/" + region.index + "/radius")
            onEdited: value => regionControls.setRadius(value)
            onGestureFinished: retouch.inspector.editor.endParameterEdit(
                continuous
                    ? "retouch/stroke/" + region.index + "/radius"
                    : "retouch/" + region.index + "/radius")
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
            value: Number(region.feather)
            toolTipText: qsTr("Soften the repair edge")
            onGestureStarted: retouch.inspector.editor.beginParameterEdit(
                continuous
                    ? "retouch/stroke/" + region.index + "/feather"
                    : "retouch/" + region.index + "/feather")
            onEdited: value => regionControls.setFeather(value)
            onGestureFinished: retouch.inspector.editor.endParameterEdit(
                continuous
                    ? "retouch/stroke/" + region.index + "/feather"
                    : "retouch/" + region.index + "/feather")
        }

        Label {
            Layout.fillWidth: true
            visible: Number(region.mode) === 1
            text: qsTr("Drag the linked source region on the image.")
            color: Theme.textMuted
            font.pixelSize: 9
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: retouch.currentTabIndex === 0
        title: qsTr("REPAIR")
        summary: retouch.inspector.editor.retouchSpots.length > 0
                || retouch.inspector.editor.retouchStrokes.length > 0
            ? qsTr("Painted")
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
                    ? qsTr("Stop painting")
                    : qsTr("Start painting")
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
                ? qsTr("Drag across the image to paint repair regions.")
                : qsTr("Select a repair region on the image to refine it.")
            color: retouch.inspector.editor.retouchPickerActive
                ? Theme.accentTextMuted : Theme.textMuted
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: retouch.inspector.editor.retouchStrokes

            delegate: RetouchRegionControls {
                region: modelData
                continuous: true
                displayIndex: modelData.index
            }
        }

        Repeater {
            model: retouch.inspector.editor.retouchSpots

            delegate: RetouchRegionControls {
                region: modelData
                continuous: false
                displayIndex: retouch.inspector.editor.retouchStrokes.length
                    + modelData.index
            }
        }
    }
}
