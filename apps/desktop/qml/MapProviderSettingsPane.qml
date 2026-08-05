pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var preferences
    property bool showDoneButton: false
    readonly property bool googleBasemapReady:
        preferences.googleApiKeyStored && preferences.googleMapTilesAllowed
    signal doneRequested()

    function prepare() {
        googleApiKeyField.clear()
        preferences.clearStatus()
        googleApiKeyField.forceActiveFocus()
    }

    function discardSecretDraft() {
        googleApiKeyField.clear()
    }

    function statusMessage() {
        switch (String(preferences.statusCode)) {
        case "api-key-saved":
            return qsTr("API key saved on this Mac.")
        case "api-key-removed":
            return qsTr("API key removed. Google service permissions are off.")
        case "invalid-api-key":
            return qsTr("Enter a valid API key without spaces.")
        case "invalid-stored-api-key":
            return qsTr("The stored API key is invalid. Replace or remove it.")
        case "api-key-required":
            return qsTr("Save an API key before allowing Google services.")
        case "secure-storage-unavailable":
            return qsTr("Shadow's local credential file is unavailable.")
        case "secret-store-failed":
            return qsTr("Shadow could not update its local credential file.")
        default:
            return ""
        }
    }

    ScrollView {
        id: settingsScroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: settingsScroll.availableWidth
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Map & Location Services")
                        color: Theme.textPrimary
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("City-level location names work offline. Add your own Google Maps Platform key only for a basemap or more precise place services.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                        lineHeight: 1.2
                    }
                }

                ShadowButton {
                    objectName: "mapProviderSettingsCloseButton"
                    visible: root.showDoneButton
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Done")
                    onClicked: root.doneRequested()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            Label {
                text: qsTr("Google Maps Platform API key")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: googleApiKeyField
                    objectName: "googleApiKeyField"
                    Layout.fillWidth: true
                    enabled: root.preferences.secureStorageAvailable
                    echoMode: TextInput.Password
                    passwordCharacter: "•"
                    selectByMouse: true
                    placeholderText: root.preferences.googleApiKeyStored
                        ? qsTr("A key is stored — enter a replacement")
                        : qsTr("Paste your API key")
                    Accessible.name: qsTr("Google Maps Platform API key")
                    onAccepted: {
                        if (googleApiKeySaveButton.enabled)
                            googleApiKeySaveButton.clicked()
                    }
                }

                ShadowButton {
                    id: googleApiKeySaveButton
                    objectName: "googleApiKeySaveButton"
                    compact: true
                    variant: ShadowButton.Primary
                    text: root.preferences.googleApiKeyStored ? qsTr("Replace") : qsTr("Save")
                    enabled: root.preferences.secureStorageAvailable
                        && googleApiKeyField.text.trim().length > 0
                    onClicked: {
                        if (root.preferences.storeGoogleApiKey(googleApiKeyField.text))
                            googleApiKeyField.clear()
                    }
                }

                ShadowButton {
                    objectName: "googleApiKeyRemoveButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Remove")
                    visible: root.preferences.googleApiKeyStored
                    onClicked: {
                        if (root.preferences.removeGoogleApiKey())
                            googleApiKeyField.clear()
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: root.preferences.googleApiKeyStored
                    ? qsTr("Stored in a user-private Shadow file on this Mac. It is not written to the catalog or backups.")
                    : qsTr("No Google API key is stored.")
                color: root.preferences.googleApiKeyStored
                    ? Theme.successText : Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            Label {
                text: qsTr("Allowed services")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Saving a key does not contact Google. Shadow may call only the services you allow, and only while the corresponding permission remains enabled.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            ShadowSwitch {
                objectName: "googleMapTilesPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Google 2D map tiles")
                enabled: root.preferences.googleApiKeyStored
                checked: root.preferences.googleMapTilesAllowed
                onToggled: root.preferences.googleMapTilesAllowed = checked
            }

            ShadowSwitch {
                objectName: "googlePlacesPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Place search and autocomplete")
                enabled: root.preferences.googleApiKeyStored
                checked: root.preferences.googlePlacesAllowed
                onToggled: root.preferences.googlePlacesAllowed = checked
            }

            ShadowSwitch {
                objectName: "googleReverseGeocodingPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Use Google for more precise place names")
                enabled: root.preferences.googleApiKeyStored
                checked: root.preferences.googleReverseGeocodingAllowed
                onToggled: root.preferences.googleReverseGeocodingAllowed = checked
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Country, region, and nearest-city lookup uses GeoNames data (CC BY 4.0) offline by default. This option may send photo coordinates to Google and falls back to offline city data if the request fails.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("No Google sign-in is required. The key's Google Cloud project must have billing and the Geocoding API enabled.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            RowLayout {
                objectName: "googleMapStyleControls"
                Layout.fillWidth: true
                visible: root.googleBasemapReady
                spacing: 4

                Label {
                    text: qsTr("Map style")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }

                ShadowButton {
                    objectName: "googleRoadmapStyleButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    selected: root.preferences.googleMapType === "roadmap"
                    text: qsTr("Road")
                    onClicked: root.preferences.googleMapType = "roadmap"
                }

                ShadowButton {
                    objectName: "googleSatelliteStyleButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    selected: root.preferences.googleMapType === "satellite"
                    text: qsTr("Satellite")
                    onClicked: root.preferences.googleMapType = "satellite"
                }

                ShadowButton {
                    objectName: "googleTerrainStyleButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    selected: root.preferences.googleMapType === "terrain"
                    text: qsTr("Terrain")
                    onClicked: root.preferences.googleMapType = "terrain"
                }

                Item { Layout.fillWidth: true }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Google tiles are requested only for the visible map, kept in a bounded memory cache according to Google's HTTP directives, and never stored for offline use.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Use a dedicated key restricted to the required APIs. Your Google project must have billing enabled; set quotas and budget alerts before use.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Label {
                Layout.fillWidth: true
                visible: root.statusMessage().length > 0
                text: root.statusMessage()
                color: root.preferences.statusCode === "api-key-saved"
                    || root.preferences.statusCode === "api-key-removed"
                    ? Theme.successText : Theme.errorText
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Item { Layout.fillHeight: true }
        }
    }
}
