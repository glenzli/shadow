pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick

// Compact remote-origin marker. Cache residency is deliberately additive:
// the network glyph always preserves origin while the check badge reports
// that the verified original is also resident on this device.
Item {
    id: indicator

    property bool cached: false
    property real iconSize: 14
    property color iconColor: Theme.textSecondary

    implicitWidth: iconSize + 4
    implicitHeight: iconSize + 2

    Accessible.role: Accessible.StaticText
    Accessible.name: cached
        ? qsTr("Remote original cached locally")
        : qsTr("Network Library source")

    ShadowIcon {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        source: "qrc:/icons/network.svg"
        color: indicator.iconColor
        size: indicator.iconSize
    }

    Rectangle {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Math.max(8, Math.round(indicator.iconSize * 0.65))
        height: width
        radius: width / 2
        visible: indicator.cached
        color: Theme.successSurface
        border.width: 1
        border.color: Theme.successBorder

        ShadowIcon {
            anchors.centerIn: parent
            source: "qrc:/icons/check.svg"
            color: Theme.successText
            size: Math.max(5, parent.width - 3)
        }
    }
}
