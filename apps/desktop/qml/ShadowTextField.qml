pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

TextField {
    id: control

    implicitHeight: Theme.controlHeight
    font.pixelSize: Theme.fontBody
    color: enabled ? Theme.textPrimary : Theme.textDisabled
    placeholderTextColor: Theme.textPlaceholder
    selectionColor: Theme.accent
    selectedTextColor: Theme.selectionForeground
    selectByMouse: true
    leftPadding: 10
    rightPadding: 10
    topPadding: 6
    bottomPadding: 6

    background: Rectangle {
        radius: Theme.controlRadius
        color: control.enabled ? Theme.control : Theme.controlDisabled
        border.width: 1
        border.color: control.activeFocus ? Theme.focusRing
            : control.enabled ? Theme.borderStrong : Theme.borderDisabled
    }
}
