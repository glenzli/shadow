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
        resetAvailable: true
        resetEnabled: retouch.regionCount > 0
        onResetRequested: retouch.inspector.editor.clearRetouch()

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 4
            spacing: 8

            ShadowIconButton {
                objectName: "retouchHealToolButton"
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
                    retouch.inspector.editor.setRetouchSourcePicking(false)
                }
            }

            ShadowIconButton {
                objectName: "retouchCloneToolButton"
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
                    if (!retouch.inspector.editor.retouchSourceSampled)
                        retouch.inspector.editor.setRetouchSourcePicking(true)
                }
            }

            Item { Layout.fillWidth: true }
        }

        ShadowSlider {
            objectName: "retouchBrushSizeSlider"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Size")
            from: 1
            to: 128
            stepSize: 1
            neutralValue: 18
            decimals: 0
            suffix: qsTr(" px")
            value: retouch.inspector.editor.retouchBrushRadius
            enabled: retouch.controlsEnabled
            toolTipText: qsTr("Radius used by the next repair · use [ and ] over the image")
            onEdited: value =>
                retouch.inspector.editor.setRetouchBrushRadius(Math.round(value))
        }

        ShadowSlider {
            objectName: "retouchBrushStrengthSlider"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Strength")
            from: 0
            to: 1
            stepSize: 0.01
            neutralValue: 1
            decimals: 0
            displayMultiplier: 100
            suffix: qsTr("%")
            value: retouch.inspector.editor.retouchBrushStrength
            enabled: retouch.controlsEnabled
            toolTipText: qsTr("Opacity used by the next repair")
            onEdited: value => retouch.inspector.editor.setRetouchBrushStrength(value)
        }

        ShadowSlider {
            objectName: "retouchBrushFeatherSlider"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Feather")
            from: 0
            to: 1
            stepSize: 0.01
            neutralValue: 0.28
            decimals: 0
            displayMultiplier: 100
            suffix: qsTr("%")
            value: retouch.inspector.editor.retouchBrushFeather
            enabled: retouch.controlsEnabled
            toolTipText: qsTr("Edge softness used by the next repair")
            onEdited: value => retouch.inspector.editor.setRetouchBrushFeather(value)
        }

        RowLayout {
            objectName: "retouchSourceAlignmentControls"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 4
            spacing: 6
            visible: retouch.inspector.editor.retouchPickerActive

            ShadowButton {
                objectName: "retouchSelectSourceButton"
                Layout.fillWidth: true
                compact: true
                selected: retouch.inspector.editor.retouchSourcePicking
                text: retouch.inspector.editor.retouchSourceSampled
                    ? qsTr("Reselect source") : qsTr("Select source")
                toolTipText: qsTr("Click, then choose a source directly on the image")
                onClicked: retouch.inspector.editor.setRetouchSourcePicking(
                    !retouch.inspector.editor.retouchSourcePicking)
            }

            ShadowButton {
                objectName: "retouchAlignedSourceButton"
                Layout.fillWidth: true
                compact: true
                selected: retouch.inspector.editor.retouchSourceAligned
                text: qsTr("Keep offset")
                toolTipText: qsTr("After the first repair, keep the same source-to-target offset for each new repair")
                onClicked: retouch.inspector.editor.setRetouchSourceAligned(true)
            }

            ShadowButton {
                objectName: "retouchFixedSourceButton"
                Layout.fillWidth: true
                compact: true
                selected: !retouch.inspector.editor.retouchSourceAligned
                text: qsTr("Reuse point")
                toolTipText: qsTr("Start every new repair from the exact sampled source point")
                onClicked: retouch.inspector.editor.setRetouchSourceAligned(false)
            }

            ShadowButton {
                objectName: "retouchClearSampledSourceButton"
                compact: true
                visible: retouch.inspector.editor.retouchSourceSampled
                text: qsTr("Clear")
                toolTipText: retouch.inspector.editor.retouchCreationMode === 1
                    ? qsTr("Clear and choose a new clone source")
                    : qsTr("Return to automatic nearby source selection")
                onClicked: retouch.inspector.editor.clearRetouchSource()
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            Layout.bottomMargin: 10
            text: retouch.inspector.editor.retouchPickerActive
                ? (retouch.inspector.editor.retouchSourcePicking
                    ? qsTr("Click the image to choose the source area.")
                    : retouch.inspector.editor.retouchSourceSampled
                    ? qsTr("Source set · drag its crosshair to move · paint the repair")
                    : retouch.inspector.editor.retouchCreationMode === 1
                        ? qsTr("Select a source before painting with Clone.")
                        : qsTr("Heal selects a nearby source automatically · use Select source to override"))
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
