pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The WebView owns the complete visible map rectangle whenever a provider is
// available. This setup surface is therefore shown only while the WebView is
// hidden; QML never overlaps native WebKit content.
Rectangle {
    id: root

    required property var workspace
    required property bool providerSelected
    required property bool providerAvailable
    required property bool providerRegionAvailable
    required property string providerName

    signal configureRequested

    visible: !providerAvailable || !providerRegionAvailable
    color: Theme.window

    Rectangle {
        objectName: "libraryMapSetupPanel"
        visible: root.visible
        anchors.centerIn: parent
        width: Math.min(440, root.width - 48)
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
                text: !root.providerRegionAvailable
                    ? qsTr("The selected map provider is unavailable in this region")
                    : root.providerSelected
                    ? qsTr("Complete the %1 map configuration").arg(root.providerName)
                    : qsTr("Choose a map service")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
            }

            Label {
                Layout.fillWidth: true
                text: !root.providerRegionAvailable
                    ? qsTr("Use Auto with a Google Maps key for photos outside mainland China. Shadow keeps one provider for the current map context instead of switching while you pan.")
                    : qsTr("The Library map uses one interactive WebView surface. Add an AMap JS API or Google Maps JavaScript API key, then choose that provider.")
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
}
