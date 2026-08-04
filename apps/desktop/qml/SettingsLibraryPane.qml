pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root

    required property var preferences
    required property var controller
    signal openLutLibraryRequested()
    signal openOpticsProfileLibraryRequested()

    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var metadataFields: [
        { key: "captured_at", title: qsTr("Capture time") },
        { key: "location", title: qsTr("Location") },
        { key: "camera", title: qsTr("Camera") },
        { key: "lens", title: qsTr("Lens") },
        { key: "exposure", title: qsTr("Shutter speed") },
        { key: "aperture", title: qsTr("Aperture") },
        { key: "iso", title: qsTr("ISO") },
        { key: "focal_length", title: qsTr("Focal length") },
        { key: "dimensions", title: qsTr("Preview dimensions") },
        { key: "focal_length_35mm", title: qsTr("35 mm equivalent") },
        { key: "raw_dimensions", title: qsTr("RAW dimensions") },
        { key: "sensor_bits", title: qsTr("Sensor bit depth") },
        { key: "cfa", title: qsTr("Color filter array") },
        { key: "dng", title: qsTr("DNG version") }
    ]

    function remoteStatusMessage() {
        switch (String(controller.remoteLibraryStatusCode)) {
        case "offline-ready":
            return controller.remoteLibraryPhotoCount > 0
                ? qsTr("Offline cache ready · %L1 remote photos").arg(
                    controller.remoteLibraryPhotoCount)
                : qsTr("No remote photos are cached on this Mac yet.")
        case "synchronizing":
            return qsTr("Updating the remote Library…")
        case "synchronized":
            return qsTr("Remote Library updated · %L1 photos").arg(
                controller.remoteLibraryPhotoCount)
        case "connection-saved":
            return qsTr("Connection saved securely.")
        case "connection-removed":
            return qsTr("Connection removed. Existing proxy thumbnails remain available offline.")
        case "connection-required":
            return qsTr("Save the server address and access token first.")
        case "token-required":
            return qsTr("The saved access token is unavailable. Enter it again.")
        case "invalid-address":
            return qsTr("Enter a server address such as 192.168.1.20:45321.")
        case "invalid-token":
            return qsTr("Enter the access token configured for the Shadow Library server.")
        case "secure-storage-unavailable":
            return qsTr("Secure credential storage is unavailable on this Mac.")
        case "secret-store-failed":
            return qsTr("The credential store could not complete the request.")
        case "sync-failed":
            return qsTr("The remote Library could not be reached. Offline thumbnails remain available.")
        case "downloading-original":
            return qsTr("Downloading and verifying the original RAW…")
        case "original-ready":
            return qsTr("The original RAW is ready for editing on this Mac.")
        case "materialize-failed":
            return qsTr("The original RAW could not be downloaded or verified.")
        case "remote-original-unavailable":
            return qsTr("This server does not currently allow original RAW downloads.")
        case "review-save-failed":
            return qsTr("A remote selection change could not be saved.")
        case "cache-load-failed":
            return qsTr("The local remote-Library cache could not be opened.")
        default:
            return ""
        }
    }

    onVisibleChanged: {
        if (visible) {
            remoteServerAddressField.text = controller.remoteLibraryServerAddress
        } else {
            remoteAccessTokenField.clear()
        }
    }

    component SettingsCard: Rectangle {
        Layout.fillWidth: true
        implicitHeight: cardContent.implicitHeight + 28
        radius: Theme.controlRadius + 2
        color: Theme.panel
        border.width: 1
        border.color: Theme.border
        default property alias content: cardContent.data

        ColumnLayout {
            id: cardContent
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 14

        Label {
            Layout.fillWidth: true
            text: qsTr("Library")
            color: Theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Control Library density, metadata presentation, and reusable photographic resources.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        SettingsCard {
            Label {
                text: qsTr("Remote Library")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Browse proxy thumbnails from another Mac. The full RAW is downloaded, resumed, and verified only when you open it for editing; edit recipes and adjusted previews stay on this Mac.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: remoteServerAddressField
                    objectName: "remoteLibraryServerAddressField"
                    Layout.fillWidth: true
                    enabled: !root.controller.remoteLibraryBusy
                    text: root.controller.remoteLibraryServerAddress
                    placeholderText: qsTr("Mac address · 192.168.1.20:45321")
                    selectByMouse: true
                    Accessible.name: qsTr("Remote Library server address")
                }

                TextField {
                    id: remoteAccessTokenField
                    objectName: "remoteLibraryAccessTokenField"
                    Layout.fillWidth: true
                    enabled: root.controller.remoteLibrarySecureStorageAvailable
                        && !root.controller.remoteLibraryBusy
                    echoMode: TextInput.Password
                    passwordCharacter: "•"
                    placeholderText: root.controller.remoteLibraryTokenStored
                        ? qsTr("Token saved — enter only to replace")
                        : qsTr("Access token")
                    selectByMouse: true
                    Accessible.name: qsTr("Remote Library access token")
                    onAccepted: {
                        if (remoteConnectionSaveButton.enabled)
                            remoteConnectionSaveButton.clicked()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ShadowButton {
                    id: remoteConnectionSaveButton
                    objectName: "remoteLibraryConnectionSaveButton"
                    compact: true
                    variant: ShadowButton.Primary
                    text: root.controller.remoteLibraryTokenStored
                        ? qsTr("Replace & Sync") : qsTr("Connect & Sync")
                    enabled: root.controller.remoteLibrarySecureStorageAvailable
                        && !root.controller.remoteLibraryBusy
                        && remoteServerAddressField.text.trim().length > 0
                        && remoteAccessTokenField.text.trim().length > 0
                    onClicked: {
                        if (root.controller.saveRemoteLibraryConnection(
                                remoteServerAddressField.text,
                                remoteAccessTokenField.text)) {
                            remoteAccessTokenField.clear()
                        }
                    }
                }

                ShadowButton {
                    objectName: "remoteLibrarySyncButton"
                    compact: true
                    text: qsTr("Sync Now")
                    enabled: root.controller.remoteLibraryTokenStored
                        && !root.controller.remoteLibraryBusy
                    onClicked: root.controller.syncRemoteLibrary()
                }

                ShadowButton {
                    objectName: "remoteLibraryRemoveButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Remove Connection")
                    visible: root.controller.remoteLibraryTokenStored
                        || root.controller.remoteLibraryServerAddress.length > 0
                    enabled: !root.controller.remoteLibraryBusy
                    onClicked: {
                        if (root.controller.removeRemoteLibraryConnection()) {
                            remoteAccessTokenField.clear()
                            remoteServerAddressField.clear()
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    visible: root.controller.remoteLibraryBusy
                    running: visible
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                }
            }

            Label {
                Layout.fillWidth: true
                text: root.controller.remoteLibraryConnected
                    && root.controller.remoteLibraryServerName.length > 0
                    ? qsTr("Server · %1").arg(root.controller.remoteLibraryServerName)
                    : root.controller.remoteLibraryConnected
                        ? qsTr("Server saved · currently offline")
                        : root.controller.remoteLibraryServerName.length > 0
                            ? qsTr("Offline cache · %1").arg(
                                root.controller.remoteLibraryServerName)
                            : qsTr("No remote Library is connected.")
                color: root.controller.remoteLibraryConnected
                    && root.controller.remoteLibraryServerName.length > 0
                    ? Theme.successText : Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.remoteStatusMessage()
                color: String(root.controller.remoteLibraryStatusCode).indexOf("failed") >= 0
                    ? Theme.dangerText : Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }
        }

        SettingsCard {
            Label {
                text: qsTr("Thumbnail size")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            ShadowSlider {
                objectName: "settingsThumbnailScaleSlider"
                Layout.fillWidth: true
                label: qsTr("Size")
                from: 96
                to: 360
                stepSize: 4
                neutralValue: 188
                fillFromMinimum: true
                decimals: 0
                suffix: " px"
                value: root.preferences.libraryThumbnailScale
                onEdited: value =>
                    root.preferences.libraryThumbnailScale = Math.round(value)
            }
        }

        SettingsCard {
            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Metadata fields")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Restore Defaults")
                    onClicked: root.preferences.resetExifFields()
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 12
                rowSpacing: 2

                Repeater {
                    model: root.metadataFields

                    ShadowCheckBox {
                        required property var modelData
                        Layout.fillWidth: true
                        text: modelData.title
                        checked: root.preferences.exifFields.indexOf(modelData.key) >= 0
                        onToggled:
                            root.preferences.setExifFieldVisible(modelData.key, checked)
                    }
                }
            }
        }

        SettingsCard {
            Label {
                text: qsTr("Photographic resources")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Manage reusable looks and optical profiles in their dedicated libraries.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ShadowButton {
                    text: qsTr("LUT Library…")
                    onClicked: root.openLutLibraryRequested()
                }

                ShadowButton {
                    text: qsTr("Optics Profiles…")
                    onClicked: root.openOpticsProfileLibraryRequested()
                }

                Item { Layout.fillWidth: true }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
