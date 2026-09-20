pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

MenuItem {
    id: control
    implicitHeight: Theme.controlHeight
    font.pixelSize: Theme.fontBody
    contentItem: Text {
        text: control.text
        font: control.font
        leftPadding: control.checkable ? 24 : 4
        color: !control.enabled ? Theme.textDisabled
            : control.highlighted ? Theme.accentSelectionText : Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.compactControlRadius
        color: control.highlighted ? Theme.accentSelectionSurface : Theme.transparent
    }
}
