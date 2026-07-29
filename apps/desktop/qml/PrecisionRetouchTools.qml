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
    required property bool selectedRegionContinuous
    required property int selectedRegionIndex

    signal regionSelectionRequested(bool continuous, int index)

    readonly property bool controlsEnabled:
        retouch.inspector.editor.active
        && !retouch.inspector.editor.stateBusy
    readonly property int strokeCount:
        retouch.inspector.editor.retouchStrokes.length
    readonly property int spotCount:
        retouch.inspector.editor.retouchSpots.length
    readonly property int regionCount: strokeCount + spotCount
    property int observedStrokeCount: 0
    property int observedSpotCount: 0
    readonly property var selectedRegion: selectedRegionContinuous
        ? (selectedRegionIndex >= 0 && selectedRegionIndex < strokeCount
            ? retouch.inspector.editor.retouchStrokes[selectedRegionIndex]
            : null)
        : (selectedRegionIndex >= 0 && selectedRegionIndex < spotCount
            ? retouch.inspector.editor.retouchSpots[selectedRegionIndex]
            : null)
    readonly property int selectedRegionDisplayIndex:
        selectedRegionContinuous
            ? selectedRegionIndex : strokeCount + selectedRegionIndex
    readonly property int regionInspectorCount:
        selectedRegion === null ? 0 : 1

    spacing: 0

    function selectRegion(continuous, index) {
        regionSelectionRequested(continuous, index)
    }

    function selectNewestRegion() {
        if (strokeCount > observedStrokeCount) {
            selectRegion(true, strokeCount - 1)
        } else if (spotCount > observedSpotCount) {
            selectRegion(false, spotCount - 1)
        } else if (strokeCount > 0) {
            selectRegion(true, strokeCount - 1)
        } else if (spotCount > 0) {
            selectRegion(false, spotCount - 1)
        } else {
            regionSelectionRequested(true, -1)
        }
    }

    function reconcileSelection() {
        const added = strokeCount > observedStrokeCount
            || spotCount > observedSpotCount
        if (added || selectedRegion === null)
            selectNewestRegion()
        observedStrokeCount = strokeCount
        observedSpotCount = spotCount
    }

    Connections {
        target: retouch.inspector.editor

        function onParametersChanged() {
            retouch.reconcileSelection()
        }
    }

    Component.onCompleted: reconcileSelection()

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

            Item { Layout.fillWidth: true }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            Layout.bottomMargin: 10
            text: retouch.inspector.editor.retouchPickerActive
                ? qsTr("Drag on the image to paint · Esc stops painting")
                : retouch.regionCount > 0
                    ? qsTr("Select a repair region below or on the image.")
                    : qsTr("Choose Heal or Clone, then paint on the image.")
            color: !retouch.controlsEnabled
                ? Theme.textDisabled
                : retouch.inspector.editor.retouchPickerActive
                    ? Theme.accentTextMuted : Theme.textMuted
            font.pixelSize: 10
            wrapMode: Text.WordWrap
            lineHeight: 1.25
        }

        PrecisionRetouchRegionPicker {
            editor: retouch.inspector.editor
            selectedContinuous: retouch.selectedRegionContinuous
            selectedIndex: retouch.selectedRegionIndex
            onRegionRequested: (continuous, index) =>
                retouch.selectRegion(continuous, index)
        }

        PrecisionRetouchRegionInspector {
            objectName: "retouchSelectedRegionInspector"
            visible: retouch.selectedRegion !== null
            editor: retouch.inspector.editor
            region: retouch.selectedRegion === null
                ? ({ "index": -1, "mode": 0, "radius": 18, "feather": 0.28 })
                : retouch.selectedRegion
            continuous: retouch.selectedRegionContinuous
            displayIndex: retouch.selectedRegionDisplayIndex
            controlsEnabled: retouch.controlsEnabled
        }
    }
}
