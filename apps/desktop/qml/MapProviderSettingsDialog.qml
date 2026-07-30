pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    required property var preferences
    required property real hostWidth
    required property real hostHeight

    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(580, Math.max(0, root.hostWidth - 48))
    height: Math.min(670, Math.max(0, root.hostHeight - 48))
    x: Math.round(((parent ? parent.width : root.hostWidth) - width) / 2)
    y: Math.round(((parent ? parent.height : root.hostHeight) - height) / 2)
    padding: 22
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: googleApiKeyField.clear()

    function present() {
        googleApiKeyField.clear();
        preferences.clearStatus();
        open();
        googleApiKeyField.forceActiveFocus();
    }

    function statusMessage() {
        switch (String(preferences.statusCode)) {
        case "api-key-saved":
            return qsTr("API key saved securely.");
        case "api-key-removed":
            return qsTr("API key removed. Google service permissions are off.");
        case "invalid-api-key":
            return qsTr("Enter a valid API key without spaces.");
        case "invalid-stored-api-key":
            return qsTr("The stored API key is invalid. Replace or remove it.");
        case "api-key-required":
            return qsTr("Save an API key before allowing Google services.");
        case "google-map-tiles-not-ready":
            return qsTr("Save an API key and allow Google 2D map tiles before selecting Google Maps.");
        case "secure-storage-unavailable":
            return qsTr("Secure credential storage is unavailable on this system.");
        case "secret-store-failed":
            return qsTr("The credential store could not complete the request.");
        default:
            return "";
        }
    }

    background: Rectangle {
        radius: Theme.controlRadius + 2
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: ScrollView {
        id: settingsScroll

        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: settingsScroll.availableWidth
            spacing: 14

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
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("OpenStreetMap remains the default. Google services are optional and use your own Google Maps Platform project.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                        lineHeight: 1.2
                    }
                }

                ShadowButton {
                    objectName: "mapProviderSettingsCloseButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Done")
                    onClicked: root.close()
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
                    placeholderText: root.preferences.googleApiKeyStored ? qsTr("A key is stored — enter a replacement") : qsTr("Paste your API key")
                    Accessible.name: qsTr("Google Maps Platform API key")
                    onAccepted: {
                        if (googleApiKeySaveButton.enabled)
                            googleApiKeySaveButton.clicked();
                    }
                }

                ShadowButton {
                    id: googleApiKeySaveButton
                    objectName: "googleApiKeySaveButton"
                    compact: true
                    variant: ShadowButton.Primary
                    text: root.preferences.googleApiKeyStored ? qsTr("Replace") : qsTr("Save")
                    enabled: root.preferences.secureStorageAvailable && googleApiKeyField.text.trim().length > 0
                    onClicked: {
                        if (root.preferences.storeGoogleApiKey(googleApiKeyField.text)) {
                            googleApiKeyField.clear();
                        }
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
                            googleApiKeyField.clear();
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: root.preferences.googleApiKeyStored ? qsTr("Stored in the operating system credential store. The key is not written to the catalog, preferences file, or backups.") : qsTr("No Google API key is stored.")
                color: root.preferences.googleApiKeyStored ? Theme.successText : Theme.textMuted
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
                text: qsTr("Saving a key does not contact Google. Shadow may call only the services you allow, and only after an explicit Google-backed action.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Switch {
                objectName: "googleMapTilesPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Google 2D map tiles")
                enabled: root.preferences.googleApiKeyStored
                checked: root.preferences.googleMapTilesAllowed
                onToggled: root.preferences.googleMapTilesAllowed = checked
            }

            Switch {
                objectName: "googlePlacesPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Place search and autocomplete")
                enabled: root.preferences.googleApiKeyStored
                checked: root.preferences.googlePlacesAllowed
                onToggled: root.preferences.googlePlacesAllowed = checked
            }

            Switch {
                objectName: "googleReverseGeocodingPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Reverse geocoding for coordinates")
                enabled: root.preferences.googleApiKeyStored
                checked: root.preferences.googleReverseGeocodingAllowed
                onToggled: root.preferences.googleReverseGeocodingAllowed = checked
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            Label {
                text: qsTr("Library map source")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                ShadowButton {
                    objectName: "openStreetMapProviderButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    selected: root.preferences.libraryMapProvider === "osm"
                    text: qsTr("OpenStreetMap")
                    onClicked: root.preferences.libraryMapProvider = "osm"
                }

                ShadowButton {
                    objectName: "googleMapsProviderButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    selected: root.preferences.libraryMapProvider === "google"
                    text: "Google Maps"
                    enabled: root.preferences.googleApiKeyStored && root.preferences.googleMapTilesAllowed
                    toolTipText: enabled ? "" : qsTr("Save an API key and allow Google 2D map tiles first.")
                    onClicked: root.preferences.libraryMapProvider = "google"
                }

                Item {
                    Layout.fillWidth: true
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.preferences.libraryMapProvider === "google"
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

                Item {
                    Layout.fillWidth: true
                }
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
                color: root.preferences.statusCode === "api-key-saved" || root.preferences.statusCode === "api-key-removed" ? Theme.successText : Theme.errorText
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.2
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }
}
