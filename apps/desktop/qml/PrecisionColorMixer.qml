pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ShadowAdjustmentSection {
    id: mixer

    required property var editor
    required property color panelRaised
    required property color panelBorder
    required property color textPrimary
    required property color textMuted
    required property color accent

    property int viewMode: 0
    property int selectedBand: 0

    readonly property var bands: [
        { "name": qsTr("Red"), "color": "#f04b4b", "hueLow": "#d94881", "hueHigh": "#f28a39", "oklchHue": 29.2339 },
        { "name": qsTr("Orange"), "color": "#f28a39", "hueLow": "#ef4c42", "hueHigh": "#e8c63c", "oklchHue": 52.9847 },
        { "name": qsTr("Yellow"), "color": "#e8c63c", "hueLow": "#f19a3d", "hueHigh": "#72bc4a", "oklchHue": 109.7692 },
        { "name": qsTr("Green"), "color": "#55b96c", "hueLow": "#b0c747", "hueHigh": "#35b6a4", "oklchHue": 142.4953 },
        { "name": qsTr("Aqua"), "color": "#32b8bd", "hueLow": "#43ae75", "hueHigh": "#3a8fdb", "oklchHue": 194.7689 },
        { "name": qsTr("Blue"), "color": "#477fdb", "hueLow": "#36a4d3", "hueHigh": "#755bd3", "oklchHue": 264.0520 },
        { "name": qsTr("Purple"), "color": "#8a5bcf", "hueLow": "#526fd9", "hueHigh": "#c34eb5", "oklchHue": 293.9376 },
        { "name": qsTr("Magenta"), "color": "#d04fa4", "hueLow": "#9856c9", "hueHigh": "#e34e73", "oklchHue": 328.3634 }
    ]

    title: qsTr("COLOR MIXER")
    summary: qsTr("OKLCH")
    toolTipText: qsTr("Adjust the hue, chroma, or Oklab lightness of each color family.")
    resetAvailable: true
    onResetRequested: editor.resetSelectedAdjustmentSection("color_mixer")

    function componentForTab(tabIndex) {
        return tabIndex === 0 ? "hue"
            : tabIndex === 1 ? "saturation" : "lightness"
    }

    function valueForBand(index, component) {
        const revision = mixer.editor.parameterRevision
        return revision >= 0
            ? mixer.editor.colorMixerValue(index, component) : 0
    }

    function trackStart(band, component) {
        if (component === "hue")
            return band.hueLow
        if (component === "saturation")
            return Theme.effectiveDark ? "#4b5055" : "#a9adb1"
        return Qt.darker(band.color, 3.2)
    }

    function trackMiddle(band, component) {
        if (component === "hue")
            return band.color
        if (component === "saturation")
            return Qt.darker(band.color, 1.35)
        return band.color
    }

    function trackEnd(band, component) {
        if (component === "hue")
            return band.hueHigh
        if (component === "saturation")
            return Qt.lighter(band.color, 1.18)
        return Qt.lighter(band.color, Theme.effectiveDark ? 1.9 : 1.55)
    }

    TabBar {
        id: mixerViewTabs
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.preferredHeight: 28
        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.surfaceSubtle
            border.color: mixer.panelBorder
        }
        onCurrentIndexChanged: mixer.viewMode = mixerViewTabs.currentIndex
        ShadowTabButton { text: qsTr("OKLCH"); compact: true }
        ShadowTabButton { text: qsTr("COLOR"); compact: true }
        ShadowTabButton { text: qsTr("CURVES"); compact: true }
    }

    TabBar {
        id: mixerTabs
        visible: mixer.viewMode === 0
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.preferredHeight: visible ? 28 : 0
        background: Item {}
        ShadowTabButton { text: qsTr("HUE"); compact: true }
        ShadowTabButton { text: qsTr("CHROMA"); compact: true }
        ShadowTabButton { text: qsTr("LIGHTNESS"); compact: true }
    }

    Repeater {
        model: mixer.viewMode === 0 ? mixer.bands : []
        delegate: ShadowSlider {
            id: hueBandSlider

            required property int index
            required property var modelData
            readonly property string component:
                mixer.componentForTab(mixerTabs.currentIndex)
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: hueBandSlider.modelData.name
            accent: hueBandSlider.modelData.color
            semanticTrack: true
            trackStartColor: mixer.trackStart(
                hueBandSlider.modelData, hueBandSlider.component)
            trackMiddleColor: mixer.trackMiddle(
                hueBandSlider.modelData, hueBandSlider.component)
            trackEndColor: mixer.trackEnd(
                hueBandSlider.modelData, hueBandSlider.component)
            from: -1.0
            to: 1.0
            neutralValue: 0.0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: mixer.valueForBand(
                hueBandSlider.index, hueBandSlider.component)
            onGestureStarted: mixer.editor.beginParameterEdit(
                "color_mixer/" + hueBandSlider.component
                    + "/" + hueBandSlider.index)
            onEdited: value => mixer.editor.setColorMixerValue(
                hueBandSlider.index, hueBandSlider.component, value)
            onGestureFinished: mixer.editor.endParameterEdit(
                "color_mixer/" + hueBandSlider.component
                    + "/" + hueBandSlider.index)
        }
    }

    RowLayout {
        visible: mixer.viewMode === 1
        Layout.fillWidth: true
        Layout.leftMargin: 20
        Layout.rightMargin: 20
        Layout.topMargin: visible ? 7 : 0
        Layout.bottomMargin: visible ? 5 : 0
        Layout.preferredHeight: visible ? 30 : 0
        spacing: 8

        Item { Layout.fillWidth: true }

        Repeater {
            model: mixer.bands

            delegate: Rectangle {
                id: bandSwatch

                required property int index
                required property var modelData
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
                Layout.alignment: Qt.AlignHCenter
                radius: 9
                color: bandSwatch.modelData.color
                border.width: mixer.selectedBand === bandSwatch.index ? 2 : 1
                border.color: mixer.selectedBand === bandSwatch.index
                    ? Theme.selectionForeground : Theme.borderStrong
                opacity: mixer.selectedBand === bandSwatch.index ? 1 : 0.72

                TapHandler {
                    onTapped: mixer.selectedBand = bandSwatch.index
                }

                ToolTip.visible: swatchHover.hovered
                ToolTip.delay: 450
                ToolTip.text: bandSwatch.modelData.name
                HoverHandler { id: swatchHover }
            }
        }

        Item { Layout.fillWidth: true }
    }

    Repeater {
        model: mixer.viewMode === 1 ? [
            { "component": "hue", "name": qsTr("Hue") },
            { "component": "saturation", "name": qsTr("Chroma") },
            { "component": "lightness", "name": qsTr("Lightness") }
        ] : []

        delegate: ShadowSlider {
            id: componentSlider

            required property int index
            required property var modelData
            readonly property int bandIndex: mixer.selectedBand
            readonly property var band: mixer.bands[componentSlider.bandIndex]
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: componentSlider.index === 0 ? 4 : 0
            label: componentSlider.modelData.name
            accent: componentSlider.band.color
            semanticTrack: true
            trackStartColor: mixer.trackStart(
                componentSlider.band, componentSlider.modelData.component)
            trackMiddleColor: mixer.trackMiddle(
                componentSlider.band, componentSlider.modelData.component)
            trackEndColor: mixer.trackEnd(
                componentSlider.band, componentSlider.modelData.component)
            from: -1.0
            to: 1.0
            neutralValue: 0.0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: mixer.valueForBand(
                componentSlider.bandIndex, componentSlider.modelData.component)
            onGestureStarted: mixer.editor.beginParameterEdit(
                "color_mixer/" + componentSlider.modelData.component
                    + "/" + componentSlider.bandIndex)
            onEdited: value => mixer.editor.setColorMixerValue(
                componentSlider.bandIndex,
                componentSlider.modelData.component,
                value)
            onGestureFinished: mixer.editor.endParameterEdit(
                "color_mixer/" + componentSlider.modelData.component
                    + "/" + componentSlider.bandIndex)
        }
    }

    HueCurveEditor {
        visible: mixer.viewMode === 2
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.topMargin: visible ? 5 : 0
        Layout.bottomMargin: visible ? 6 : 0
        Layout.preferredHeight: visible ? implicitHeight : 0
        controller: mixer.editor
        bands: mixer.bands
        panelColor: mixer.panelRaised
        plotColor: Theme.chrome
        borderColor: mixer.panelBorder
        textColor: mixer.textPrimary
        mutedTextColor: mixer.textMuted
        accentColor: mixer.accent
    }
}
