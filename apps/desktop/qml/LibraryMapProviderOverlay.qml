pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var workspace
    required property bool googleProviderSelected
    required property bool googleProviderAvailable
    required property string googleStatusMessage

    signal configureRequested()

    Rectangle {
        id: providerPanel
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 14
        radius: 6
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.border
        implicitWidth: providerContent.implicitWidth + 14
        implicitHeight: providerContent.implicitHeight + 12

        ColumnLayout {
            id: providerContent
            anchors.centerIn: parent
            spacing: 4

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: root.googleProviderSelected ? "Google Maps" : qsTr("Basemap unavailable")
                color: root.googleProviderSelected ? Theme.textPrimary : Theme.textMuted
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("%L1 photos in view").arg(root.workspace.controller.libraryMapPhotoCount)
                color: Theme.textSecondary
                font.pixelSize: 10
            }
        }
    }

    BusyIndicator {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 14
        width: 30
        height: 30
        visible: root.workspace.controller.libraryMapBusy || (root.googleProviderSelected && root.workspace.googleMapTilesService.busy)
        running: visible
    }

    Rectangle {
        id: setupPanel
        objectName: "libraryMapSetupPanel"
        anchors.centerIn: parent
        visible: !root.googleProviderAvailable
        width: Math.min(420, root.width - 48)
        height: setupContent.implicitHeight + 32
        radius: 10
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong

        ColumnLayout {
            id: setupContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: 16
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Configure a map service")
                color: Theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("To display the basemap, add a Google Maps Platform API key and allow Google 2D map tiles.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            ShadowButton {
                objectName: "libraryMapConfigureButton"
                Layout.alignment: Qt.AlignHCenter
                variant: ShadowButton.Primary
                text: qsTr("Open map settings")
                onClicked: root.configureRequested()
            }
        }
    }

    Rectangle {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 12
        anchors.bottomMargin: 10
        visible: root.googleProviderSelected
        radius: 4
        color: Theme.panelRaised
        opacity: 0.92
        border.width: 1
        border.color: Theme.border
        width: Math.min(root.width - 24, Math.max(googleMapsLabel.implicitWidth, googleCopyrightLabel.implicitWidth) + 14)
        height: googleAttribution.implicitHeight + 10

        Column {
            id: googleAttribution
            anchors.centerIn: parent
            width: parent.width - 14
            spacing: 1

            Label {
                id: googleMapsLabel
                width: parent.width
                text: "Google Maps"
                color: Theme.textPrimary
                font.pixelSize: 12
                font.weight: Font.Normal
            }

            Label {
                id: googleCopyrightLabel
                width: parent.width
                visible: text.length > 0
                text: String(root.workspace.googleMapTilesService.copyrightText)
                color: Theme.textMuted
                font.pixelSize: 9
                wrapMode: Text.Wrap
            }
        }
    }

    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 18
        width: Math.min(520, parent.width - 48)
        visible: root.googleProviderSelected && root.googleStatusMessage.length > 0
        text: root.googleStatusMessage
        color: Theme.errorText
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
    }

}
