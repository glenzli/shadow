pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Slider {
    id: control

    property real neutralValue: from
    property bool showNeutralMarker: neutralValue > from && neutralValue < to
    // Most adjustments visualize distance from their neutral value. Amount
    // controls instead fill from the logical minimum while keeping an
    // independent reset value (for example AI strength resets to 100%).
    property bool fillFromMinimum: false
    property color accent: Theme.accent
    property bool semanticTrack: false
    property color trackStartColor: Theme.track
    property color trackMiddleColor: Theme.track
    property color trackEndColor: Theme.track
    property string toolTipText: ""

    readonly property real logicalNeutralPosition: to === from
        ? 0 : Math.max(0, Math.min(1, (neutralValue - from) / (to - from)))
    readonly property real neutralPosition: mirrored
        ? 1 - logicalNeutralPosition : logicalNeutralPosition
    readonly property real minimumVisualPosition: mirrored ? 1 : 0
    readonly property real fillStartPosition: fillFromMinimum
        ? Math.min(visualPosition, minimumVisualPosition)
        : Math.min(visualPosition, neutralPosition)
    readonly property real fillEndPosition: fillFromMinimum
        ? Math.max(visualPosition, minimumVisualPosition)
        : Math.max(visualPosition, neutralPosition)

    signal resetRequested(real value)

    implicitWidth: 112
    implicitHeight: 22
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    function requestNeutralReset() {
        const boundedNeutral = Math.max(from, Math.min(to, neutralValue))
        if (!enabled || Math.abs(value - boundedNeutral) < 0.0000001)
            return
        resetRequested(boundedNeutral)
    }

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: control.semanticTrack ? 5 : 3
        radius: height / 2
        color: control.enabled ? Theme.track : Theme.borderDisabled
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0
                color: control.enabled && control.semanticTrack
                    ? control.trackStartColor : Theme.track
            }
            GradientStop {
                position: 0.5
                color: control.enabled && control.semanticTrack
                    ? control.trackMiddleColor : Theme.track
            }
            GradientStop {
                position: 1
                color: control.enabled && control.semanticTrack
                    ? control.trackEndColor : Theme.track
            }
        }

        Rectangle {
            visible: !control.semanticTrack
            x: control.fillStartPosition * parent.width
            width: (control.fillEndPosition - control.fillStartPosition)
                * parent.width
            height: parent.height
            radius: parent.radius
            color: control.enabled ? control.accent : Theme.textDisabled
        }

        Rectangle {
            visible: control.showNeutralMarker
            x: Math.round(control.neutralPosition * parent.width) - width / 2
            y: -2
            width: 1
            height: parent.height + 4
            color: control.enabled ? Theme.textMuted : Theme.textDisabled
        }
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onDoubleTapped: control.requestNeutralReset()
    }

    handle: Rectangle {
        x: control.leftPadding
            + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 12
        implicitHeight: 12
        radius: 6
        color: !control.enabled
            ? Theme.buttonDisabledSurface
            : control.pressed ? Theme.accentHover : Theme.panelRaised
        border.width: control.visualFocus ? 2 : 1
        border.color: !control.enabled
            ? Theme.borderDisabled
            : control.visualFocus ? Theme.focusRing : control.accent

        Behavior on color {
            ColorAnimation { duration: 80 }
        }
    }

    ToolTip {
        id: toolTip

        parent: control
        visible: control.enabled && control.hovered
            && !control.pressed && control.toolTipText.length > 0
        delay: 450
        timeout: 4000
        text: control.toolTipText
        x: Math.round((control.width - width) / 2)
        y: control.height + 6

        contentItem: Label {
            text: toolTip.text
            color: Theme.textPrimary
            font.pixelSize: Theme.fontMeta
            font.weight: Font.Medium
        }

        background: Rectangle {
            radius: Theme.compactControlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
    }
}
