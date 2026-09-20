pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the compact, time-aware ordinary-life collection entry. Travel keeps
// its destination hierarchy in its own component below.
ColumnLayout {
    id: daily
    objectName: "reviewDailyCollection"

    required property var workspace
    visible: workspace.personalProfile.hasLivingPlaces
    spacing: 3

    function activateDaily() {
        workspace.applyDailyCollection()
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 6
        Layout.bottomMargin: 3
        text: qsTr("DAILY")
        color: Theme.textMuted
        font.pixelSize: Theme.fontCaption
        font.weight: Font.DemiBold
        font.letterSpacing: 1.2
    }

    Rectangle {
        id: dailyRow
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        radius: Theme.compactControlRadius
        color: daily.workspace.isDailyCollectionActive()
            ? Theme.accentSurface
            : dailyMouse.containsMouse ? Theme.buttonGhostHover : Theme.transparent

        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: 3
            anchors.verticalCenter: parent.verticalCenter
            width: 2
            height: 16
            radius: 1
            color: daily.workspace.isDailyCollectionActive() ? Theme.accent : Theme.transparent
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 9
            spacing: 7

            ShadowIcon {
                source: "qrc:/icons/location-pin.svg"
                size: 14
                color: Theme.textSecondary
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Daily")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
            }
            BusyIndicator {
                visible: daily.workspace.controller.travelCollectionsBusy
                running: visible
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
            }
            Label {
                visible: !daily.workspace.controller.travelCollectionsBusy
                text: qsTr("%L1").arg(daily.workspace.controller.dailyPhotoCount)
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }
        }

        MouseArea {
            id: dailyMouse
            objectName: "dailyCollectionMouse"
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: daily.activateDaily()
        }
    }
}
