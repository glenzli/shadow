pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Compact navigation for all photo-local repair regions. Parameters belong to
// PrecisionRetouchRegionInspector, so the collection never multiplies sliders.
ColumnLayout {
    id: regionPicker
    objectName: "retouchRegionPicker"

    required property var editor
    required property bool selectedContinuous
    required property int selectedIndex

    signal regionRequested(bool continuous, int index)

    readonly property int strokeCount: editor.retouchStrokes.length
    readonly property int regionCount:
        strokeCount + editor.retouchSpots.length
    readonly property int focusRingGutter: 2
    readonly property int regionButtonSize: 34
    readonly property int regionButtonSpacing: 6
    readonly property int selectedDisplayIndex: {
        const collection = selectedContinuous
            ? editor.retouchStrokes : editor.retouchSpots
        for (let itemIndex = 0; itemIndex < collection.length; ++itemIndex) {
            if (Number(collection[itemIndex].index) === selectedIndex) {
                return selectedContinuous
                    ? itemIndex : strokeCount + itemIndex
            }
        }
        return -1
    }

    Layout.fillWidth: true
    Layout.leftMargin: 14
    Layout.rightMargin: 14
    Layout.bottomMargin: regionCount > 0 ? 4 : 0
    visible: regionCount > 0
    spacing: 6

    function revealSelected() {
        regionFlickable.ensureDisplayIndexVisible(selectedDisplayIndex)
    }

    function queueRevealSelected() {
        Qt.callLater(regionPicker.revealSelected)
    }

    onSelectedContinuousChanged: queueRevealSelected()
    onSelectedIndexChanged: queueRevealSelected()
    Component.onCompleted: queueRevealSelected()

    Connections {
        target: regionPicker.editor

        function onParametersChanged() {
            regionPicker.queueRevealSelected()
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("REGIONS")
        color: Theme.textMuted
        font.pixelSize: 9
        font.weight: Font.DemiBold
        font.letterSpacing: 0.65
    }

    Flickable {
        id: regionFlickable
        objectName: "retouchRegionFlickable"

        Layout.fillWidth: true
        Layout.preferredHeight: 38
        contentWidth: regionPicker.regionCount > 0
            ? regionPicker.focusRingGutter * 2
                + regionPicker.regionCount
                    * regionPicker.regionButtonSize
                + (regionPicker.regionCount - 1)
                    * regionPicker.regionButtonSpacing
            : 0
        contentHeight: height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick

        function ensureDisplayIndexVisible(displayIndex) {
            if (displayIndex < 0 || width <= 0)
                return
            const buttonLeft = regionPicker.focusRingGutter
                + displayIndex * (
                    regionPicker.regionButtonSize
                        + regionPicker.regionButtonSpacing
                )
            const itemLeft = buttonLeft - regionPicker.focusRingGutter
            const itemRight = buttonLeft + regionPicker.regionButtonSize
                + regionPicker.focusRingGutter
            const maximumContentX = Math.max(0, contentWidth - width)
            if (itemLeft < contentX) {
                contentX = Math.max(0, itemLeft)
            } else if (itemRight > contentX + width) {
                contentX = Math.min(
                    maximumContentX,
                    itemRight - width
                )
            }
        }

        onWidthChanged: regionPicker.queueRevealSelected()

        Row {
            id: regionRow

            x: regionPicker.focusRingGutter
            y: regionPicker.focusRingGutter
            height: regionPicker.regionButtonSize
            spacing: regionPicker.regionButtonSpacing

            Repeater {
                id: strokeRepeater
                model: regionPicker.editor.retouchStrokes

                delegate: ShadowIconButton {
                    required property var modelData
                    required property int index
                    readonly property int regionIndex:
                        Number(modelData.index)
                    readonly property int displayIndex: index
                    objectName: "retouchRegionButton"

                    buttonSize: regionPicker.regionButtonSize
                    iconSize: 18
                    source: Number(modelData.mode) === 0
                        ? "qrc:/icons/heal.svg"
                        : "qrc:/icons/clone.svg"
                    selected: regionPicker.selectedContinuous
                        && regionPicker.selectedIndex === regionIndex
                    Accessible.selected: selected
                    toolTipText: qsTr("Repair region %1").arg(
                        displayIndex + 1)
                    accessibleName: toolTipText
                    onClicked: regionPicker.regionRequested(
                        true, regionIndex)
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            regionFlickable.ensureDisplayIndexVisible(
                                displayIndex)
                        }
                    }
                    Component.onCompleted: {
                        if (selected)
                            regionPicker.queueRevealSelected()
                    }
                }
            }

            Repeater {
                id: spotRepeater
                model: regionPicker.editor.retouchSpots

                delegate: ShadowIconButton {
                    required property var modelData
                    required property int index
                    readonly property int regionIndex:
                        Number(modelData.index)
                    readonly property int displayIndex:
                        regionPicker.strokeCount + index
                    objectName: "retouchRegionButton"

                    buttonSize: regionPicker.regionButtonSize
                    iconSize: 18
                    source: Number(modelData.mode) === 0
                        ? "qrc:/icons/heal.svg"
                        : "qrc:/icons/clone.svg"
                    selected: !regionPicker.selectedContinuous
                        && regionPicker.selectedIndex === regionIndex
                    Accessible.selected: selected
                    toolTipText: qsTr("Repair region %1").arg(
                        displayIndex + 1)
                    accessibleName: toolTipText
                    onClicked: regionPicker.regionRequested(
                        false, regionIndex)
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            regionFlickable.ensureDisplayIndexVisible(
                                displayIndex)
                        }
                    }
                    Component.onCompleted: {
                        if (selected)
                            regionPicker.queueRevealSelected()
                    }
                }
            }
        }
    }
}
