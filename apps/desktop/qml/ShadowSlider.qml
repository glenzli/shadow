pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: field

    required property string label
    property alias value: slider.value
    property alias from: slider.from
    property alias to: slider.to
    property alias stepSize: slider.stepSize
    property real neutralValue: from
    property int decimals: 2
    property real displayMultiplier: 1.0
    property string suffix: ""
    property color accent: Theme.accent
    property bool semanticTrack: false
    property color trackStartColor: Theme.track
    property color trackMiddleColor: Theme.track
    property color trackEndColor: Theme.track
    property string startLabel: ""
    property string endLabel: ""
    property int endpointLabelWidth: 38
    property color textPrimary: Theme.textPrimary
    property color textMuted: Theme.textMuted
    property string toolTipText: ""
    // Keep one stable label column across languages. Common photographic
    // terms fit directly; exceptional labels elide and expose their full text
    // on hover instead of stealing width from the adjustment track.
    property int labelWidth: Math.max(56, Math.min(72, Math.round(width * 0.24)))
    property int valueWidth: 54
    property bool gestureActive: false
    readonly property bool hasEndpointLabels:
        startLabel.length > 0 || endLabel.length > 0

    readonly property string formattedValue: qsTr("%1%2")
        .arg(Number(slider.value * displayMultiplier).toLocaleString(
            Qt.locale(), "f", decimals))
        .arg(suffix)

    signal edited(real value)
    signal gestureStarted()
    signal gestureFinished()

    implicitHeight: 24
    activeFocusOnTab: false

    function beginGesture() {
        if (gestureActive)
            return
        gestureActive = true
        gestureStarted()
    }

    function finishGesture() {
        keyboardSettle.stop()
        if (!gestureActive)
            return
        gestureActive = false
        gestureFinished()
    }

    function resetToNeutral() {
        if (!enabled || Math.abs(value - neutralValue) < 0.0000001)
            return
        finishGesture()
        beginGesture()
        edited(neutralValue)
        finishGesture()
    }

    onEnabledChanged: {
        if (!enabled)
            finishGesture()
    }

    Timer {
        id: keyboardSettle
        interval: 240
        repeat: false
        onTriggered: field.finishGesture()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 4

        Label {
            id: fieldLabel

            Layout.preferredWidth: visible ? field.labelWidth : 0
            Layout.minimumWidth: 0
            visible: !field.hasEndpointLabels
            text: field.label
            color: field.enabled ? field.textPrimary : Theme.textDisabled
            font.pixelSize: 10
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter

            HoverHandler { id: labelHover }
            ToolTip.visible: labelHover.hovered
                && (fieldLabel.truncated || field.toolTipText.length > 0)
            ToolTip.delay: 500
            ToolTip.text: field.toolTipText.length > 0 ? field.toolTipText : field.label
        }

        Item {
            visible: field.hasEndpointLabels
            Layout.preferredWidth: field.valueWidth
            Layout.minimumWidth: field.valueWidth
        }

        Label {
            visible: field.startLabel.length > 0
            Layout.preferredWidth: field.endpointLabelWidth
            Layout.minimumWidth: field.endpointLabelWidth
            text: field.startLabel
            color: field.enabled ? field.trackStartColor : Theme.textDisabled
            font.pixelSize: 9
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
        }

        ShadowInlineSlider {
            id: slider

            Layout.fillWidth: true
            Layout.minimumWidth: 64
            neutralValue: field.neutralValue
            accent: field.accent
            semanticTrack: field.semanticTrack
            trackStartColor: field.trackStartColor
            trackMiddleColor: field.trackMiddleColor
            trackEndColor: field.trackEndColor
            snapMode: Slider.SnapAlways
            enabled: field.enabled
            Accessible.name: field.label
            Accessible.description: field.formattedValue

            onMoved: {
                if (!pressed) {
                    field.beginGesture()
                    keyboardSettle.restart()
                }
                field.edited(value)
            }
            onPressedChanged: {
                if (pressed) {
                    keyboardSettle.stop()
                    field.beginGesture()
                } else if (field.gestureActive) {
                    field.finishGesture()
                }
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onDoubleTapped: field.resetToNeutral()
            }
        }

        Label {
            visible: field.endLabel.length > 0
            Layout.preferredWidth: field.endpointLabelWidth
            Layout.minimumWidth: field.endpointLabelWidth
            text: field.endLabel
            color: field.enabled ? field.trackEndColor : Theme.textDisabled
            font.pixelSize: 9
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
        }

        Label {
            Layout.preferredWidth: field.valueWidth
            Layout.minimumWidth: field.valueWidth
            text: field.formattedValue
            color: field.enabled ? field.textMuted : Theme.textDisabled
            font.family: "Menlo"
            font.pixelSize: 9
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideLeft
        }
    }
}
