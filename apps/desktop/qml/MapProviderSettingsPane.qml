pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var preferences
    property bool showDoneButton: false
    readonly property bool libraryMapReady:
        preferences.libraryMapProvider !== "none"
    readonly property bool amapServiceReady:
        preferences.amapWebServiceKeyStored
    signal doneRequested()

    function prepare() {
        googleApiKeyField.clear()
        amapWebKeyField.clear()
        amapJsKeyField.clear()
        amapSecurityCodeField.clear()
        preferences.clearStatus()
        amapWebKeyField.forceActiveFocus()
    }

    function discardSecretDraft() {
        googleApiKeyField.clear()
        amapWebKeyField.clear()
        amapJsKeyField.clear()
        amapSecurityCodeField.clear()
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
        case "amap-web-key-saved":
            return qsTr("AMap Web Service key saved on this Mac.")
        case "amap-web-key-removed":
            return qsTr("AMap Web Service key removed. AMap place services are off.")
        case "amap-js-credentials-saved":
            return qsTr("AMap JS API credentials saved on this Mac.")
        case "amap-js-credentials-removed":
            return qsTr("AMap JS API credentials removed.")
        case "invalid-amap-web-key":
            return qsTr("Enter a valid AMap Web Service key without spaces.")
        case "invalid-stored-amap-web-key":
            return qsTr("The stored AMap Web Service key is invalid. Replace or remove it.")
        case "invalid-amap-js-credentials":
            return qsTr("Enter both the AMap JS API key and security code without spaces.")
        case "amap-web-key-required":
            return qsTr("Save an AMap Web Service key before allowing AMap place services.")
        case "amap-js-credentials-required":
            return qsTr("Save AMap JS API credentials before choosing AMap for the Library map.")
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
                        text: qsTr("City-level location names work offline. Add your own AMap or Google credentials only for a basemap, place search, or more precise place names.")
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
                text: qsTr("AMap")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Use the Web Service key for place search and precise location names in mainland China. Shadow converts coordinates only at the AMap boundary; Catalog GPS data remains WGS84.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Label {
                text: qsTr("Web Service key")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: amapWebKeyField
                    objectName: "amapWebServiceKeyField"
                    Layout.fillWidth: true
                    enabled: root.preferences.secureStorageAvailable
                    echoMode: TextInput.Password
                    passwordCharacter: "•"
                    selectByMouse: true
                    placeholderText: root.preferences.amapWebServiceKeyStored
                        ? qsTr("A key is stored — enter a replacement")
                        : qsTr("Paste the AMap Web Service key")
                    Accessible.name: qsTr("AMap Web Service key")
                    onAccepted: {
                        if (amapWebKeySaveButton.enabled)
                            amapWebKeySaveButton.clicked()
                    }
                }

                ShadowButton {
                    id: amapWebKeySaveButton
                    objectName: "amapWebServiceKeySaveButton"
                    compact: true
                    variant: ShadowButton.Primary
                    text: root.preferences.amapWebServiceKeyStored ? qsTr("Replace") : qsTr("Save")
                    enabled: root.preferences.secureStorageAvailable
                        && amapWebKeyField.text.trim().length > 0
                    onClicked: {
                        if (root.preferences.storeAmapWebServiceKey(amapWebKeyField.text))
                            amapWebKeyField.clear()
                    }
                }

                ShadowButton {
                    objectName: "amapWebServiceKeyRemoveButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Remove")
                    visible: root.preferences.amapWebServiceKeyStored
                    onClicked: {
                        if (root.preferences.removeAmapWebServiceKey())
                            amapWebKeyField.clear()
                    }
                }
            }

            ShadowSwitch {
                objectName: "amapPlacesPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("AMap place search")
                enabled: root.preferences.amapWebServiceKeyStored
                checked: root.preferences.amapPlacesAllowed
                onToggled: root.preferences.amapPlacesAllowed = checked
            }

            ShadowSwitch {
                objectName: "amapReverseGeocodingPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Use AMap for precise place names in mainland China")
                enabled: root.preferences.amapWebServiceKeyStored
                checked: root.preferences.amapReverseGeocodingAllowed
                onToggled: root.preferences.amapReverseGeocodingAllowed = checked
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("These options may send a search phrase or photo coordinates to AMap. Failed requests fall back to Shadow's offline city data.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Label {
                text: qsTr("JS API key and security code")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: amapJsKeyField
                    objectName: "amapJsApiKeyField"
                    Layout.fillWidth: true
                    enabled: root.preferences.secureStorageAvailable
                    echoMode: TextInput.Password
                    passwordCharacter: "•"
                    selectByMouse: true
                    placeholderText: root.preferences.amapJsCredentialsStored
                        ? qsTr("Credentials are stored — enter replacements")
                        : qsTr("JS API key")
                    Accessible.name: qsTr("AMap JS API key")
                }

                TextField {
                    id: amapSecurityCodeField
                    objectName: "amapSecurityJsCodeField"
                    Layout.fillWidth: true
                    enabled: root.preferences.secureStorageAvailable
                    echoMode: TextInput.Password
                    passwordCharacter: "•"
                    selectByMouse: true
                    placeholderText: qsTr("securityJsCode")
                    Accessible.name: qsTr("AMap JS API security code")
                }

                ShadowButton {
                    id: amapJsCredentialsSaveButton
                    objectName: "amapJsCredentialsSaveButton"
                    compact: true
                    variant: ShadowButton.Primary
                    text: root.preferences.amapJsCredentialsStored ? qsTr("Replace") : qsTr("Save")
                    enabled: root.preferences.secureStorageAvailable
                        && amapJsKeyField.text.trim().length > 0
                        && amapSecurityCodeField.text.trim().length > 0
                    onClicked: {
                        if (root.preferences.storeAmapJsCredentials(
                                amapJsKeyField.text, amapSecurityCodeField.text)) {
                            amapJsKeyField.clear()
                            amapSecurityCodeField.clear()
                        }
                    }
                }

                ShadowButton {
                    objectName: "amapJsCredentialsRemoveButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Remove")
                    visible: root.preferences.amapJsCredentialsStored
                    onClicked: {
                        if (root.preferences.removeAmapJsCredentials()) {
                            amapJsKeyField.clear()
                            amapSecurityCodeField.clear()
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: root.preferences.amapJsCredentialsStored
                    ? qsTr("Stored for the official AMap interactive WebView renderer. The credentials are not written to the catalog or backups.")
                    : qsTr("No AMap JS API credentials are stored.")
                color: root.preferences.amapJsCredentialsStored
                    ? Theme.successText : Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            ShadowSwitch {
                objectName: "amapLibraryMapSwitch"
                Layout.fillWidth: true
                text: qsTr("Use AMap for the Library map")
                enabled: root.preferences.amapJsCredentialsStored
                checked: root.preferences.libraryMapProvider === "amap"
                onToggled: root.preferences.libraryMapProvider = checked ? "amap" : "none"
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            Label {
                text: qsTr("Google Maps Platform")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
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
                objectName: "googleLibraryMapSwitch"
                Layout.fillWidth: true
                text: qsTr("Use Google Maps for the Library map")
                enabled: root.preferences.googleApiKeyStored
                checked: root.preferences.libraryMapProvider === "google"
                onToggled: root.preferences.libraryMapProvider = checked ? "google" : "none"
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
                objectName: "libraryMapStyleControls"
                Layout.fillWidth: true
                visible: root.libraryMapReady
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
                    selected: root.preferences.mapStyle === "roadmap"
                    text: qsTr("Road")
                    onClicked: root.preferences.mapStyle = "roadmap"
                }

                ShadowButton {
                    objectName: "googleSatelliteStyleButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    selected: root.preferences.mapStyle === "satellite"
                    text: qsTr("Satellite")
                    onClicked: root.preferences.mapStyle = "satellite"
                }

                Item { Layout.fillWidth: true }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Google and AMap share one Qt WebView map surface. The selected provider loads its official JavaScript map only while the Library map is open; Shadow does not maintain a separate tile cache.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("For Google, enable the Maps JavaScript API rather than the Map Tiles API. Use a dedicated restricted key, quotas, and budget alerts.")
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
                    || root.preferences.statusCode === "amap-web-key-saved"
                    || root.preferences.statusCode === "amap-web-key-removed"
                    || root.preferences.statusCode === "amap-js-credentials-saved"
                    || root.preferences.statusCode === "amap-js-credentials-removed"
                    ? Theme.successText : Theme.errorText
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Item { Layout.fillHeight: true }
        }
    }
}
