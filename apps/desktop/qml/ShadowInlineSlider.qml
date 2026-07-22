pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Slider {
    id: control

    property real neutralValue: from
    property bool showNeutralMarker: neutralValue > from && neutralValue < to
    property color accent: Theme.accent
    property bool semanticTrack: false
    property color trackStartColor: Theme.track
    property color trackMiddleColor: Theme.track
    property color trackEndColor: Theme.track

    implicitWidth: 112
    implicitHeight: 22
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

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

        readonly property real logicalNeutralPosition: control.to === control.from
            ? 0 : Math.max(0, Math.min(1,
                (control.neutralValue - control.from) / (control.to - control.from)))
        readonly property real neutralPosition: control.mirrored
            ? 1 - logicalNeutralPosition : logicalNeutralPosition

        Rectangle {
            visible: !control.semanticTrack
            x: Math.min(control.visualPosition, parent.neutralPosition) * parent.width
            width: Math.abs(control.visualPosition - parent.neutralPosition) * parent.width
            height: parent.height
            radius: parent.radius
            color: control.enabled ? control.accent : Theme.textDisabled
        }

        Rectangle {
            visible: control.showNeutralMarker
            x: Math.round(parent.neutralPosition * parent.width) - width / 2
            y: -2
            width: 1
            height: parent.height + 4
            color: control.enabled ? Theme.textMuted : Theme.textDisabled
        }
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
}
