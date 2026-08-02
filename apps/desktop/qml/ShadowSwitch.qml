pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Switch {
    id: control

    property bool compact: false
    property color accentColor: Theme.accent
    property string accessibleName: text
    readonly property bool shadowStyled: true

    spacing: compact ? 7 : 9
    padding: 0
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0
    implicitWidth: Math.max(indicator.implicitWidth, contentItem.implicitWidth)
    implicitHeight: compact ? 24 : 28
    hoverEnabled: enabled
    focusPolicy: enabled ? Qt.StrongFocus : Qt.NoFocus
    Accessible.name: accessibleName

    indicator: Rectangle {
        implicitWidth: control.compact ? 32 : 34
        implicitHeight: control.compact ? 17 : 18
        x: control.width - width
        y: Math.round((control.height - height) / 2)
        radius: height / 2
        color: !control.enabled
            ? Theme.controlDisabled
            : control.checked
                ? Theme.switchOnSurface : Theme.switchOffSurface
        border.width: 1
        border.color: !control.enabled
            ? Theme.borderDisabled
            : control.visualFocus
                ? Theme.focusRing
                : control.checked
                    ? Theme.switchOnBorder
                    : control.hovered
                        ? Theme.borderEmphasis : Theme.switchOffBorder

        Rectangle {
            width: control.compact ? 11 : 12
            height: width
            y: Math.round((parent.height - height) / 2)
            x: control.checked ? parent.width - width - 3 : 3
            radius: width / 2
            color: !control.enabled
                ? Theme.textDisabledQuiet
                : control.checked ? control.accentColor : Theme.textMuted
            scale: control.down ? 0.92 : 1

            Behavior on x {
                NumberAnimation { duration: 100; easing.type: Easing.OutCubic }
            }
            Behavior on scale {
                NumberAnimation { duration: 70 }
            }
        }
    }

    contentItem: Label {
        rightPadding: control.indicator.implicitWidth + control.spacing
        text: control.text
        color: control.enabled ? Theme.textSecondary : Theme.textDisabled
        font.pixelSize: control.compact ? Theme.fontMeta : Theme.fontBody
        font.weight: Font.Medium
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: Theme.compactControlRadius
        color: control.enabled && control.hovered
            ? Theme.buttonGhostHover : Theme.transparent
    }
}
