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
    property color textPrimary: Theme.textPrimary
    property color textMuted: Theme.textMuted
    property int labelWidth: Math.max(48, Math.min(58, Math.round(width * 0.19)))
    property int valueWidth: 54
    property bool gestureActive: false

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

            Layout.preferredWidth: field.labelWidth
            Layout.minimumWidth: 0
            text: field.label
            color: field.enabled ? field.textPrimary : Theme.textDisabled
            font.pixelSize: 10
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter

            HoverHandler { id: labelHover }
            ToolTip.visible: labelHover.hovered && fieldLabel.truncated
            ToolTip.delay: 500
            ToolTip.text: field.label
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
