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

    readonly property bool controlsEnabled:
        retouch.inspector.editor.active
        && !retouch.inspector.editor.stateBusy

    spacing: 0

    component RetouchRegionControls: ColumnLayout {
        id: regionControls
        required property var region
        required property bool continuous
        required property int displayIndex

        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.topMargin: 8
        Layout.bottomMargin: 4
        enabled: retouch.controlsEnabled
        spacing: 6

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
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Region %1").arg(
                    regionControls.displayIndex + 1)
                color: regionControls.enabled
                    ? Theme.textSecondary : Theme.textDisabled
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }

            ShadowIconButton {
                buttonSize: 30
                iconSize: 17
                source: "qrc:/icons/heal.svg"
                selected: Number(regionControls.region.mode) === 0
                toolTipText: qsTr(
                    "Blend a defect from its surrounding pixels")
                accessibleName: qsTr("Heal") + " · "
                    + qsTr("Region %1").arg(
                        regionControls.displayIndex + 1)
                onClicked: regionControls.setMode(0)
            }

            ShadowIconButton {
                buttonSize: 30
                iconSize: 17
                source: "qrc:/icons/clone.svg"
                selected: Number(regionControls.region.mode) === 1
                toolTipText: qsTr("Copy a same-shaped nearby source")
                accessibleName: qsTr("Clone") + " · "
                    + qsTr("Region %1").arg(
                        regionControls.displayIndex + 1)
                onClicked: regionControls.setMode(1)
            }

            ShadowIconButton {
                buttonSize: 30
                iconSize: 16
                source: "qrc:/icons/trash.svg"
                variant: ShadowIconButton.Danger
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
            color: regionControls.enabled
                ? Theme.textMuted : Theme.textDisabled
            font.pixelSize: 9
            lineHeight: 1.2
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
        sectionEnabled: retouch.controlsEnabled

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 4
            spacing: 8

            ShadowIconButton {
                buttonSize: 36
                iconSize: 19
                source: "qrc:/icons/heal.svg"
                selected: retouch.inspector.editor.retouchCreationMode === 0
                toolTipText: qsTr("Blend a defect from its surrounding pixels")
                accessibleName: qsTr("Heal")
                enabled: retouch.controlsEnabled
                onClicked: {
                    retouch.inspector.editor.setRetouchCreationMode(0)
                    retouch.inspector.editor.setRetouchPickerActive(true)
                }
            }

            ShadowIconButton {
                buttonSize: 36
                iconSize: 19
                source: "qrc:/icons/clone.svg"
                selected: retouch.inspector.editor.retouchCreationMode === 1
                toolTipText: qsTr("Copy a same-shaped nearby source")
                accessibleName: qsTr("Clone")
                enabled: retouch.controlsEnabled
                onClicked: {
                    retouch.inspector.editor.setRetouchCreationMode(1)
                    retouch.inspector.editor.setRetouchPickerActive(true)
                }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 22
                color: Theme.border
            }

            ShadowIconButton {
                buttonSize: 36
                iconSize: 19
                source: "qrc:/icons/brush.svg"
                selected: retouch.inspector.editor.retouchPickerActive
                toolTipText: retouch.inspector.editor.retouchPickerActive
                    ? qsTr("Stop painting")
                    : qsTr("Start painting")
                accessibleName: toolTipText
                enabled: retouch.controlsEnabled
                onClicked: retouch.inspector.editor.setRetouchPickerActive(
                    !retouch.inspector.editor.retouchPickerActive)
            }

            Item { Layout.fillWidth: true }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            Layout.bottomMargin: 10
            text: retouch.inspector.editor.retouchPickerActive
                ? qsTr("Drag across the image to paint repair regions.")
                : qsTr("Select a repair region on the image to refine it.")
            color: !retouch.controlsEnabled
                ? Theme.textDisabled
                : retouch.inspector.editor.retouchPickerActive
                    ? Theme.accentTextMuted : Theme.textMuted
            font.pixelSize: 10
            wrapMode: Text.WordWrap
            lineHeight: 1.25
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
