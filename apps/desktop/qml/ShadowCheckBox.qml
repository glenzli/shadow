pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

CheckBox {
    id: control

    property bool compact: false
    property string accessibleName: text
    readonly property bool shadowStyled: true

    spacing: compact ? 7 : 8
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

    indicator: Item {
        implicitWidth: control.compact ? 15 : 16
        implicitHeight: implicitWidth
        x: 0
        y: Math.round((control.height - height) / 2)

        Rectangle {
            anchors.fill: parent
            radius: control.compact ? 3 : 4
            color: !control.enabled
                ? Theme.controlDisabled
                : control.checked
                    ? Theme.accentSurface : Theme.controlQuiet
            border.width: 1
            border.color: !control.enabled
                ? Theme.borderDisabled
                : control.visualFocus
                    ? Theme.focusRing
                    : control.checked
                        ? Theme.accentBorder
                        : control.hovered
                            ? Theme.borderEmphasis : Theme.borderStrong
            scale: control.down ? 0.94 : 1

            ShadowIcon {
                anchors.centerIn: parent
                visible: control.checked
                source: "qrc:/icons/check.svg"
                color: control.enabled ? Theme.accent : Theme.textDisabled
                size: control.compact ? 9 : 10
            }

            Behavior on scale {
                NumberAnimation { duration: 70 }
            }
        }
    }

    contentItem: Label {
        leftPadding: control.indicator.implicitWidth + control.spacing
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
