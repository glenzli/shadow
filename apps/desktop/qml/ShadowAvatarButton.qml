pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

AbstractButton {
    id: control

    required property var profile
    property string toolTipText: qsTr("Personal profile")

    implicitWidth: 30
    implicitHeight: 30
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.name: toolTipText

    background: Rectangle {
        radius: width / 2
        color: control.down ? Theme.buttonGhostPressed
            : control.hovered ? Theme.buttonGhostHover : Theme.panelRaised
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? Theme.focusRing : Theme.borderStrong
    }

    contentItem: Item {
        ShadowRoundedImage {
            anchors.fill: parent
            anchors.margins: 2
            visible: String(control.profile.avatarUrl).length > 0
            source: control.profile.avatarUrl
            radius: width / 2
            fillMode: Image.PreserveAspectCrop
            requestedSourceSize: Qt.size(56, 56)
        }

        Label {
            anchors.centerIn: parent
            visible: String(control.profile.avatarUrl).length === 0
            text: control.profile.avatarInitial
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
        }
    }

    ToolTip {
        visible: control.hovered
        delay: 450
        text: control.toolTipText
    }
}
