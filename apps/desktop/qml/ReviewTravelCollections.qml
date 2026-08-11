pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: travel
    objectName: "reviewTravelCollections"

    required property var workspace
    property string expandedCountryKey: ""
    visible: workspace.personalProfile.hasLivingPlaces
    spacing: 3

    function activateAllTravel() {
        workspace.applyTravelCollection("", "")
        workspace.commitLibraryScopeSelection()
    }

    Label {
        Layout.fillWidth: true
        Layout.topMargin: 6
        Layout.bottomMargin: 3
        text: qsTr("TRAVEL")
        color: Theme.textMuted
        font.pixelSize: 9
        font.weight: Font.DemiBold
        font.letterSpacing: 1.2
    }

    Rectangle {
        id: allTravelRow
        Layout.fillWidth: true
        Layout.preferredHeight: 32
        radius: Theme.compactControlRadius
        color: travel.workspace.isTravelCollectionActive("", "")
            ? Theme.accentSurface
            : allTravelMouse.containsMouse ? Theme.buttonGhostHover : Theme.transparent

        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: 3
            anchors.verticalCenter: parent.verticalCenter
            width: 2
            height: 16
            radius: 1
            color: travel.workspace.isTravelCollectionActive("", "")
                ? Theme.accent : Theme.transparent
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 9
            spacing: 7

            ShadowIcon {
                source: "qrc:/icons/map.svg"
                size: 14
                color: Theme.textSecondary
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Travel")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
            }
            BusyIndicator {
                visible: travel.workspace.controller.travelCollectionsBusy
                running: visible
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
            }
            Label {
                visible: !travel.workspace.controller.travelCollectionsBusy
                text: qsTr("%L1").arg(travel.workspace.controller.travelPhotoCount)
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }
        }

        MouseArea {
            id: allTravelMouse
            objectName: "allTravelCollectionMouse"
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: travel.activateAllTravel()
        }
    }

    Label {
        Layout.fillWidth: true
        visible: String(travel.workspace.controller.travelCollectionsErrorText).length > 0
        text: travel.workspace.controller.travelCollectionsErrorText
        color: Theme.errorText
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.Wrap
    }

    Repeater {
        model: travel.workspace.controller.travelGroups

        delegate: ColumnLayout {
            id: countryGroup
            required property var modelData
            readonly property string countryKey: String(modelData.key)
            readonly property bool expanded: travel.expandedCountryKey === countryKey
            Layout.fillWidth: true
            spacing: 2

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                radius: Theme.compactControlRadius
                color: travel.workspace.isTravelCollectionActive(countryGroup.countryKey, "")
                    ? Theme.accentSurface
                    : countryMouse.containsMouse ? Theme.buttonGhostHover : Theme.transparent

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 9
                    anchors.rightMargin: 9
                    spacing: 5

                    Label {
                        text: countryGroup.expanded ? "⌄" : "›"
                        color: Theme.textMuted
                        font.pixelSize: 14
                    }
                    Label {
                        Layout.fillWidth: true
                        text: String(countryGroup.modelData.label)
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    Label {
                        text: qsTr("%L1").arg(countryGroup.modelData.photoCount)
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                    }
                }

                MouseArea {
                    id: countryMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        travel.expandedCountryKey = countryGroup.expanded
                            ? "" : countryGroup.countryKey
                        travel.workspace.applyTravelCollection(countryGroup.countryKey, "")
                    }
                }
            }

            Repeater {
                model: countryGroup.expanded ? countryGroup.modelData.destinations : []

                delegate: Rectangle {
                    id: destinationRow
                    required property var modelData
                    readonly property string destinationKey: String(modelData.key)
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.preferredHeight: 28
                    radius: Theme.compactControlRadius
                    color: travel.workspace.isTravelCollectionActive(
                               countryGroup.countryKey, destinationKey)
                        ? Theme.accentSurface
                        : destinationMouse.containsMouse
                            ? Theme.buttonGhostHover : Theme.transparent

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 9
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: String(destinationRow.modelData.label)
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontMeta
                            elide: Text.ElideRight
                        }
                        Label {
                            text: qsTr("%L1").arg(destinationRow.modelData.photoCount)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                        }
                    }

                    MouseArea {
                        id: destinationMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            travel.workspace.applyTravelCollection(
                                countryGroup.countryKey,
                                destinationRow.destinationKey)
                            travel.workspace.commitLibraryScopeSelection()
                        }
                    }
                }
            }
        }
    }
}
