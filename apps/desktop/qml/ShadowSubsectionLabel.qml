pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property string text
    property string toolTipText: ""

    implicitHeight: 22
    Layout.fillWidth: true

    Label {
        id: caption
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        text: root.text
        color: Theme.textMuted
        font.pixelSize: 9
        font.weight: Font.DemiBold
        font.letterSpacing: 0.7
        elide: Text.ElideRight

        HoverHandler { id: captionHover }
        ToolTip.visible: captionHover.hovered && root.toolTipText.length > 0
        ToolTip.delay: 500
        ToolTip.timeout: 5000
        ToolTip.text: root.toolTipText
    }
}
