pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ShadowAdjustmentSection {
    id: pointColorSection

    required property var editor
    required property var analysisScope
    required property bool previewFrameReady
    required property string readyPreviewGeneration
    required property bool comparisonActive
    required property color accent

    property bool skinCheckPending: false
    readonly property bool pickerAvailable: editor.active
        && editor.pointColors.length < 16
        && previewFrameReady
        && readyPreviewGeneration.length > 0
        && !comparisonActive

    function fineValue(key) {
        const revision = editor.parameterRevision
        return revision >= 0 ? editor.parameterValue(key) : 0
    }

    function toggleSkinCheck() {
        analysisScope.scopeMode = analysisScope.vectorscopeScope
        analysisScope.skinGuideVisible = true
        if (editor.pointColorScopeActive) {
            skinCheckPending = false
            editor.pointColorScopeActive = false
        } else if (editor.pointColorScopeAvailable) {
            skinCheckPending = false
            editor.pointColorScopeActive = true
        } else {
            skinCheckPending = true
            editor.setPointColorPickerActive(true)
        }
    }

    function applySkinGuideNudge(requestedHueNudge) {
        const current = fineValue("color_range_hue")
        const next = Math.max(-180, Math.min(180,
            current + requestedHueNudge))
        if (next === current)
            return
        editor.beginParameterEdit("skin_guide/point_color_hue")
        editor.setParameterValue("color_range_hue", next)
        editor.endParameterEdit("skin_guide/point_color_hue")
    }

    Connections {
        target: pointColorSection.editor

        function onParametersChanged() {
            if (!pointColorSection.skinCheckPending
                    || pointColorSection.editor.selectedPointColorIndex < 0)
                return
            pointColorSection.skinCheckPending = false
            pointColorSection.analysisScope.scopeMode
                = pointColorSection.analysisScope.vectorscopeScope
            pointColorSection.analysisScope.skinGuideVisible = true
            pointColorSection.editor.pointColorScopeActive = true
        }

        function onPointColorPickerActiveChanged() {
            if (pointColorSection.skinCheckPending
                    && !pointColorSection.editor.pointColorPickerActive
                    && pointColorSection.editor.selectedPointColorIndex < 0) {
                pointColorSection.skinCheckPending = false
            }
        }

        function onSelectedGradeNodeChanged() {
            pointColorSection.skinCheckPending = false
        }
    }

    Layout.fillWidth: true
    title: qsTr("POINT COLOR")
    toolTipText: qsTr("Use the eyedropper to build one or more precise Oklch color ranges from the image.")
    resetAvailable: true
    onResetRequested: editor.resetSelectedAdjustmentSection("point_color")

    PrecisionPointColorSampleBar {
        Layout.fillWidth: true
        editor: pointColorSection.editor
        pickerAvailable: pointColorSection.pickerAvailable
        accent: pointColorSection.accent
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.topMargin: 2
        Layout.bottomMargin: 2
        spacing: 8

        ShadowIconButton {
            buttonSize: 30
            iconSize: 16
            source: "qrc:/icons/scopes.svg"
            selected: pointColorSection.editor.pointColorScopeActive
            toolTipText: qsTr("Sample a representative skin midtone, freeze its diagnostic pixels, and inspect shadow, midtone, and highlight alignment in the Vectorscope.")
            accessibleName: selected
                ? qsTr("SKIN REFERENCE LOCKED")
                : qsTr("SKIN CHECK")
            onClicked: pointColorSection.toggleSkinCheck()
        }

        ShadowButton {
            compact: true
            minimumButtonWidth: 0
            variant: selected ? ShadowButton.Tinted : ShadowButton.Ghost
            selected: pointColorSection.editor.pointColorScopeActive
            text: selected
                ? qsTr("SKIN REFERENCE LOCKED")
                : qsTr("SKIN CHECK")
            toolTipText: qsTr("Sample a representative skin midtone, freeze its diagnostic pixels, and inspect shadow, midtone, and highlight alignment in the Vectorscope.")
            onClicked: pointColorSection.toggleSkinCheck()
        }

        Item { Layout.fillWidth: true }
    }

    Rectangle {
        id: skinGuideNudge

        readonly property int minimumMatchedPixels: 96
        readonly property real maximumToneDeviationSpread: 15
        readonly property real guideDeviation:
            pointColorSection.analysisScope.displayScopeSkinGuideDeviation
        readonly property var shadows:
            pointColorSection.analysisScope.skinToneRange("Shadows")
        readonly property var midtones:
            pointColorSection.analysisScope.skinToneRange("Midtones")
        readonly property var highlights:
            pointColorSection.analysisScope.skinToneRange("Highlights")
        readonly property real toneDeviationSpread: {
            const ranges = [shadows, midtones, highlights]
            let largest = 0
            for (let first = 0; first < ranges.length; ++first) {
                if (!ranges[first].available)
                    continue
                for (let second = first + 1;
                        second < ranges.length; ++second) {
                    if (!ranges[second].available)
                        continue
                    const wrapped = (ranges[first].deviation
                        - ranges[second].deviation + 540) % 360 - 180
                    largest = Math.max(largest, Math.abs(wrapped))
                }
            }
            return largest
        }
        readonly property bool hasSufficientSample:
            pointColorSection.analysisScope.displayScopeMatchedPixels
                >= minimumMatchedPixels
        readonly property bool toneSplit:
            toneDeviationSpread > maximumToneDeviationSpread
        readonly property real requestedHueNudge: Math.max(-12, Math.min(
            12, -guideDeviation))
        readonly property int roundedGuideDeviation:
            Math.round(guideDeviation)
        readonly property int roundedHueNudge:
            Math.round(requestedHueNudge)
        readonly property bool nudgeAvailable: hasSufficientSample
            && !toneSplit
            && Math.abs(requestedHueNudge) >= 0.5

        visible: pointColorSection.editor.pointColorScopeActive
            && pointColorSection.analysisScope.displayScopeCentroidAvailable
            && pointColorSection.editor.selectedPointColorIndex >= 0
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.bottomMargin: visible ? 5 : 0
        Layout.preferredHeight: visible ? 31 : 0
        radius: 4
        color: Qt.rgba(0.92, 0.55, 0.37, 0.09)
        border.width: 1
        border.color: Qt.rgba(0.92, 0.55, 0.37, 0.35)

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 5
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: skinGuideNudge.toneSplit
                    ? qsTr("TONE SPLIT · USE SEPARATE NODES")
                    : !skinGuideNudge.hasSufficientSample
                        ? qsTr("SAMPLE TOO SMALL · REFINE POINT COLOR")
                        : !skinGuideNudge.nudgeAvailable
                            ? qsTr("ALIGNED · NO NUDGE NEEDED")
                            : qsTr("SKIN GUIDE Δ %1").arg(
                                (skinGuideNudge.roundedGuideDeviation > 0
                                    ? "+" : "")
                                + skinGuideNudge.roundedGuideDeviation + "°")
                color: skinGuideNudge.nudgeAvailable
                    ? Theme.textSecondary : "#e6a36c"
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            RowLayout {
                visible: skinGuideNudge.nudgeAvailable
                spacing: 4

                Label {
                    text: (skinGuideNudge.roundedHueNudge > 0 ? "+" : "")
                        + skinGuideNudge.roundedHueNudge + "°"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontMeta
                    font.weight: Font.DemiBold
                }

                ShadowIconButton {
                    buttonSize: 30
                    iconSize: 16
                    source: "qrc:/icons/edit.svg"
                    enabled: !pointColorSection.editor.stateBusy
                    toolTipText: qsTr("Apply the guide direction as a limited starting hue correction for this Point Color. It is undoable and does not change the node mask or global color.")
                    accessibleName: toolTipText
                    onClicked: pointColorSection.applySkinGuideNudge(
                        skinGuideNudge.requestedHueNudge)
                }
            }
        }
    }

    Rectangle {
        visible: pointColorSection.editor.selectedPointColorIndex >= 0
        Layout.fillWidth: true
        Layout.leftMargin: 26
        Layout.rightMargin: 26
        Layout.topMargin: visible ? 5 : 0
        Layout.bottomMargin: visible ? 7 : 0
        Layout.preferredHeight: visible ? 8 : 0
        radius: 4
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.000000; color: "#eb6f9a" }
            GradientStop { position: 0.081205; color: "#ff0000" }
            GradientStop { position: 0.147180; color: "#ff8000" }
            GradientStop { position: 0.304914; color: "#ffff00" }
            GradientStop { position: 0.395820; color: "#00ff00" }
            GradientStop { position: 0.541025; color: "#00ffff" }
            GradientStop { position: 0.733478; color: "#0000ff" }
            GradientStop { position: 0.816493; color: "#8000ff" }
            GradientStop { position: 0.912121; color: "#ff00ff" }
            GradientStop { position: 1.000000; color: "#eb6f9a" }
        }

        Rectangle {
            x: Math.max(0, Math.min(parent.width - width,
                ((pointColorSection.fineValue("color_range_center")
                    % 360) + 360) % 360 / 360 * parent.width - width / 2))
            y: -3
            width: 4
            height: parent.height + 6
            radius: 2
            color: Theme.selectionForeground
            border.color: Theme.accentHandleBorder
        }
    }

    Repeater {
        model: [
            { "key": "color_range_center", "name": qsTr("Target hue (OKLCh)"), "from": 0, "to": 360, "neutral": 0, "step": 1, "scale": 1, "suffix": "°" },
            { "key": "color_range_width", "name": qsTr("Range"), "from": 1, "to": 180, "neutral": 30, "step": 1, "scale": 1, "suffix": "°" },
            { "key": "color_range_softness", "name": qsTr("Softness"), "from": 0, "to": 1, "neutral": 0.5, "step": 0.01, "scale": 100, "suffix": "%" },
            { "key": "color_range_hue", "name": qsTr("Hue shift"), "from": -180, "to": 180, "neutral": 0, "step": 1, "scale": 1, "suffix": "°" },
            { "key": "color_range_saturation", "name": qsTr("Chroma"), "from": -1, "to": 1, "neutral": 0, "step": 0.01, "scale": 100, "suffix": "%" },
            { "key": "color_range_lightness", "name": qsTr("Lightness"), "from": -1, "to": 1, "neutral": 0, "step": 0.01, "scale": 100, "suffix": "%" }
        ]

        delegate: ShadowSlider {
            required property var modelData

            visible: pointColorSection.editor.selectedPointColorIndex >= 0
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: modelData.name
            from: modelData.from
            to: modelData.to
            neutralValue: modelData.neutral
            stepSize: modelData.step
            decimals: 0
            displayMultiplier: modelData.scale
            suffix: modelData.suffix
            value: pointColorSection.fineValue(modelData.key)
            enabled: pointColorSection.editor.selectedPointColorIndex >= 0
            onGestureStarted:
                pointColorSection.editor.beginParameterEdit(modelData.key)
            onEdited: value =>
                pointColorSection.editor.setParameterValue(
                    modelData.key, value)
            onGestureFinished:
                pointColorSection.editor.endParameterEdit(modelData.key)
        }
    }
}
