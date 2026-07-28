pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Photo-local deterministic repair. Heal preserves a nearby donor's texture
// and gradient-blends it into the target; Clone copies the donor directly.
// Both expose the editable source region, and painting persists one ordered
// stroke whose radius is swept into continuous coverage.
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
            if (regionControls.continuous) {
                retouch.inspector.editor.setRetouchStrokeMode(
                    regionControls.region.index, mode)
            } else {
                retouch.inspector.editor.setRetouchSpotMode(
                    regionControls.region.index, mode)
            }
        }

        function remove() {
            if (regionControls.continuous) {
                retouch.inspector.editor.removeRetouchStroke(
                    regionControls.region.index)
            } else {
                retouch.inspector.editor.removeRetouchSpot(
                    regionControls.region.index)
            }
        }

        function setRadius(value) {
            if (regionControls.continuous) {
                retouch.inspector.editor.setRetouchStrokeRadius(
                    regionControls.region.index, Math.round(value))
            } else {
                retouch.inspector.editor.setRetouchSpotRadius(
                    regionControls.region.index, Math.round(value))
            }
        }

        function setFeather(value) {
            if (regionControls.continuous) {
                retouch.inspector.editor.setRetouchStrokeFeather(
                    regionControls.region.index, value)
            } else {
                retouch.inspector.editor.setRetouchSpotFeather(
                    regionControls.region.index, value)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: qsTr("Region %1").arg(
                    regionControls.displayIndex + 1)
                color: Theme.textSecondary
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }

            ShadowButton {
                compact: true
                minimumButtonWidth: 42
                text: qsTr("Heal")
                selected: Number(regionControls.region.mode) === 0
                onClicked: regionControls.setMode(0)
            }

            ShadowButton {
                compact: true
                minimumButtonWidth: 45
                text: qsTr("Clone")
                selected: Number(regionControls.region.mode) === 1
                onClicked: regionControls.setMode(1)
            }

            ShadowIconButton {
                buttonSize: 24
                iconSize: 15
                source: "qrc:/icons/trash.svg"
                toolTipText: qsTr("Remove region %1").arg(
                    regionControls.displayIndex + 1)
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
            value: regionControls.region.radius
            toolTipText: qsTr("Full-resolution repair radius")
            onGestureStarted: retouch.inspector.editor.beginParameterEdit(
                regionControls.continuous
                    ? "retouch/stroke/" + regionControls.region.index
                        + "/radius"
                    : "retouch/" + regionControls.region.index + "/radius")
            onEdited: value => regionControls.setRadius(value)
            onGestureFinished: retouch.inspector.editor.endParameterEdit(
                regionControls.continuous
                    ? "retouch/stroke/" + regionControls.region.index
                        + "/radius"
                    : "retouch/" + regionControls.region.index + "/radius")
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
            value: Number(regionControls.region.feather)
            toolTipText: qsTr("Soften the repair edge")
            onGestureStarted: retouch.inspector.editor.beginParameterEdit(
                regionControls.continuous
                    ? "retouch/stroke/" + regionControls.region.index
                        + "/feather"
                    : "retouch/" + regionControls.region.index + "/feather")
            onEdited: value => regionControls.setFeather(value)
            onGestureFinished: retouch.inspector.editor.endParameterEdit(
                regionControls.continuous
                    ? "retouch/stroke/" + regionControls.region.index
                        + "/feather"
                    : "retouch/" + regionControls.region.index + "/feather")
        }

        Label {
            Layout.fillWidth: true
            visible: Number(regionControls.region.mode) === 1
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
                required property var modelData
                region: modelData
                continuous: true
                displayIndex: modelData.index
            }
        }

        Repeater {
            model: retouch.inspector.editor.retouchSpots

            delegate: RetouchRegionControls {
                required property var modelData
                region: modelData
                continuous: false
                displayIndex: retouch.inspector.editor.retouchStrokes.length
                    + modelData.index
            }
        }
    }
}
