pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Button {
    id: control

    property color panelColor: "#1b2025"
    property color pressedColor: "#2a3037"
    property color borderColor: "#343b43"
    property color enabledTextColor: "#bdc4ca"
    property color disabledTextColor: "#606a74"

    implicitHeight: 28
    focusPolicy: Qt.StrongFocus

    background: Rectangle {
        radius: 3
        color: control.down ? control.pressedColor : control.panelColor
        border.color: control.activeFocus ? control.enabledTextColor : control.borderColor
        opacity: control.enabled ? 1.0 : 0.65
    }

    contentItem: Label {
        text: control.text
        color: control.enabled ? control.enabledTextColor : control.disabledTextColor
        font.pixelSize: 8
        font.weight: Font.Bold
        font.letterSpacing: 0.5
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
