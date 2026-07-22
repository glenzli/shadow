pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Button {
    id: control

    property color labelColor: Theme.transparent
    property bool selected: false
    property string toolTipText: ""
    property string accessibleName: toolTipText
    property int buttonSize: 24

    implicitWidth: buttonSize
    implicitHeight: buttonSize
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: enabled
    focusPolicy: enabled ? Qt.StrongFocus : Qt.NoFocus
    Accessible.name: accessibleName
    Accessible.checked: selected

    background: Item {
        implicitWidth: control.buttonSize
        implicitHeight: control.buttonSize

        Rectangle {
            anchors.centerIn: parent
            width: control.selected ? 18 : control.hovered ? 16 : 14
            height: width
            radius: width / 2
            color: control.enabled ? control.labelColor : Theme.textDisabledQuiet
            opacity: control.enabled ? 1.0 : 0.45
            border.width: control.selected || control.visualFocus ? 2 : 0
            border.color: control.selected ? Theme.textPrimary : Theme.focusRing

            Behavior on width {
                NumberAnimation { duration: 90 }
            }
            Behavior on opacity {
                NumberAnimation { duration: 90 }
            }
        }
    }

    ToolTip {
        parent: control
        visible: control.enabled && control.hovered
            && control.toolTipText.length > 0
        delay: 450
        timeout: 4000
        text: control.toolTipText
        x: Math.round((control.width - width) / 2)
        y: control.height + 6

        contentItem: Label {
            text: control.toolTipText
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
