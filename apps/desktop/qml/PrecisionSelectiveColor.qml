pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ShadowAdjustmentSection {
    id: selectiveColor

    required property var editor
    required property color panelBorder

    property int selectedTarget: 0

    readonly property var targets: [
        { "name": qsTr("Reds"), "color": "#ef5b62" },
        { "name": qsTr("Yellows"), "color": "#e5bf45" },
        { "name": qsTr("Greens"), "color": "#4fb473" },
        { "name": qsTr("Cyans"), "color": "#37bdc7" },
        { "name": qsTr("Blues"), "color": "#5484d8" },
        { "name": qsTr("Magentas"), "color": "#cf5aa9" },
        { "name": qsTr("White"), "color": "#edf0f3" },
        { "name": qsTr("Neutral"), "color": "#8d98a5" },
        { "name": qsTr("Black"), "color": "#27313b" }
    ]
    readonly property var cmykComponents: [
        { "name": qsTr("Cyan"), "component": 0 },
        { "name": qsTr("Magenta"), "component": 1 },
        { "name": qsTr("Yellow"), "component": 2 },
        { "name": qsTr("Black"), "component": 3 }
    ]

    title: qsTr("SELECTIVE COLOR")
    summary: qsTr("OKLAB · CMYK")
    toolTipText: qsTr("Choose an Oklab color family, then apply a Photoshop-style CMYK correction.")

    function componentValue(targetIndex, componentIndex) {
        const revision = selectiveColor.editor.parameterRevision
        return revision >= 0
            ? selectiveColor.editor.selectiveColorValue(
                targetIndex, componentIndex)
            : 0
    }

    function relativeMethod() {
        const revision = selectiveColor.editor.parameterRevision
        return revision >= 0
            ? selectiveColor.editor.selectiveColorRelative()
            : true
    }

    function fineValue(key) {
        const revision = selectiveColor.editor.parameterRevision
        return revision >= 0
            ? selectiveColor.editor.parameterValue(key)
            : 0
    }

    function trackStart(componentIndex) {
        if (componentIndex === 0)
            return "#e96870"
        if (componentIndex === 1)
            return "#58a976"
        if (componentIndex === 2)
            return "#5f88d4"
        return Theme.effectiveDark ? "#dce3ea" : "#ffffff"
    }

    function trackEnd(componentIndex) {
        if (componentIndex === 0)
            return "#37bdc7"
        if (componentIndex === 1)
            return "#cf5aa9"
        if (componentIndex === 2)
            return "#e5bf45"
        return Theme.effectiveDark ? "#1c242c" : "#202830"
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.topMargin: 3
        Layout.bottomMargin: 3
        spacing: 4

        Item { Layout.fillWidth: true }

        Repeater {
            model: selectiveColor.targets

            delegate: ShadowColorLabelButton {
                required property int index
                required property var modelData
                buttonSize: 22
                labelColor: modelData.color
                selected: selectiveColor.selectedTarget === index
                toolTipText: modelData.name
                accessibleName: toolTipText
                onClicked: selectiveColor.selectedTarget = index
            }
        }

        Item { Layout.fillWidth: true }
    }

    TabBar {
        id: methodTabs
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.topMargin: 3
        Layout.preferredHeight: 28
        currentIndex: selectiveColor.relativeMethod() ? 0 : 1
        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.surfaceSubtle
            border.color: selectiveColor.panelBorder
        }
        onCurrentIndexChanged: {
            const relative = currentIndex === 0
            if (relative === selectiveColor.relativeMethod())
                return
            selectiveColor.editor.beginParameterEdit("selective_color/method")
            selectiveColor.editor.setSelectiveColorRelative(relative)
            selectiveColor.editor.endParameterEdit("selective_color/method")
        }
        ShadowTabButton {
            text: qsTr("RELATIVE")
            compact: true
            toolTipText: qsTr("Scale the existing CMYK component")
        }
        ShadowTabButton {
            text: qsTr("ABSOLUTE")
            compact: true
            toolTipText: qsTr("Add or remove a fixed CMYK amount")
        }
    }

    ShadowSlider {
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.topMargin: 2
        label: qsTr("Lightness lock")
        toolTipText: qsTr("Preserve the source Oklab lightness after CMYK correction. 0% follows Selective Color; 100% changes hue and chroma only.")
        from: 0.0
        to: 1.0
        neutralValue: 0.0
        stepSize: 0.01
        decimals: 0
        displayMultiplier: 100
        suffix: "%"
        value: selectiveColor.fineValue(
            "selective_color_lightness_protection")
        onGestureStarted: selectiveColor.editor.beginParameterEdit(
            "selective_color/lightness_protection")
        onEdited: value => selectiveColor.editor.setParameterValue(
            "selective_color_lightness_protection", value)
        onGestureFinished: selectiveColor.editor.endParameterEdit(
            "selective_color/lightness_protection")
    }

    Repeater {
        model: selectiveColor.cmykComponents

        delegate: ShadowSlider {
            required property var modelData
            readonly property int targetIndex: selectiveColor.selectedTarget
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: modelData.name
            semanticTrack: true
            trackStartColor: selectiveColor.trackStart(modelData.component)
            trackMiddleColor: Theme.track
            trackEndColor: selectiveColor.trackEnd(modelData.component)
            from: -1.0
            to: 1.0
            neutralValue: 0.0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: selectiveColor.componentValue(targetIndex, modelData.component)
            onGestureStarted: selectiveColor.editor.beginParameterEdit(
                "selective_color/" + targetIndex
                    + "/" + modelData.component)
            onEdited: value => selectiveColor.editor.setSelectiveColorValue(
                targetIndex, modelData.component, value)
            onGestureFinished: selectiveColor.editor.endParameterEdit(
                "selective_color/" + targetIndex
                    + "/" + modelData.component)
        }
    }
}
