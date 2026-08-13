pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root

    required property var controller
    required property var mapController
    required property var placeSearchService
    property bool nativeWebMapAllowed: true
    property var targets: []
    property bool mapActivated: false
    signal configureMapRequested()

    title: qsTr("Set Photo Location")
    width: 920
    height: 680
    minimumWidth: 700
    minimumHeight: 560
    color: Theme.window
    flags: Qt.Dialog
    modality: Qt.WindowModal

    function synchronizeMap() {
        root.mapController.setPlacementActive(locationState.open)
        root.mapController.setPendingCoordinate(
            locationState.hasCoordinate,
            locationState.latitude,
            locationState.longitude)
    }

    function activateMap() {
        if (root.mapActivated)
            return
        root.mapActivated = true
        root.mapController.setLanguage(Qt.locale().name)
        if (locationState.hasCoordinate) {
            root.mapController.navigateToContext(
                locationState.latitude, locationState.longitude, 13)
        } else {
            root.mapController.beginMapContext(
                root.mapController.centerLatitude,
                root.mapController.centerLongitude)
        }
        root.mapController.active = true
        root.synchronizeMap()
    }

    function deactivateMap() {
        if (!root.mapActivated)
            return
        root.mapController.setPlacementActive(false)
        root.mapController.setPendingCoordinate(false, 0, 0)
        root.mapController.active = false
        root.placeSearchService.clear()
        root.mapActivated = false
    }

    function requestDismiss() {
        if (locationState.busy)
            return false
        root.close()
        return true
    }

    function present(selectedTargets, initialHasCoordinate,
                     initialLatitude, initialLongitude, initialPlaceName) {
        return presentWithSource(
                    selectedTargets, initialHasCoordinate, initialLatitude,
                    initialLongitude, initialPlaceName, "manual-map")
    }

    function presentWithSource(selectedTargets, initialHasCoordinate,
                               initialLatitude, initialLongitude, initialPlaceName,
                               initialSourceLabel) {
        root.targets = selectedTargets || []
        if (!locationState.presentWithSource(
                root.targets, initialHasCoordinate, initialLatitude,
                initialLongitude, initialPlaceName, initialSourceLabel))
            return false
        latitudeField.text = locationState.hasCoordinate
            ? Number(locationState.latitude).toLocaleString(
                  Qt.locale("C"), "f", 7) : ""
        longitudeField.text = locationState.hasCoordinate
            ? Number(locationState.longitude).toLocaleString(
                  Qt.locale("C"), "f", 7) : ""
        placeField.text = locationState.placeName
        policyBox.currentIndex = 0
        show()
        raise()
        requestActivate()
        Qt.callLater(root.activateMap)
        return true
    }

    function acceptManualFields() {
        const latitude = Number.fromLocaleString(
            Qt.locale("C"), latitudeField.text.trim())
        const longitude = Number.fromLocaleString(
            Qt.locale("C"), longitudeField.text.trim())
        if (!locationState.proposeCoordinate(
                latitude, longitude, placeField.text.trim(), "manual-map"))
            return false
        root.mapController.navigateToContext(latitude, longitude, 13)
        root.synchronizeMap()
        return true
    }

    onVisibilityChanged: {
        if (visible)
            Qt.callLater(root.activateMap)
        else
            root.deactivateMap()
    }
    onClosing: close => {
        if (locationState.busy) {
            close.accepted = false
            return
        }
        locationState.close()
        root.deactivateMap()
    }

    LibraryLocationBatchState {
        id: locationState
        controller: root.controller
        onLocationApplied: count => {
            applyResultTimer.restart()
        }
    }

    Timer {
        id: applyResultTimer
        interval: 850
        repeat: false
        onTriggered: root.close()
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: root.visible && !locationState.busy
        onActivated: root.requestDismiss()
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 16

        ColumnLayout {
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            spacing: 12

            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Set Photo Location")
                    color: Theme.textPrimary
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }

                ShadowIconButton {
                    objectName: "locationBatchCloseButton"
                    source: "qrc:/icons/close.svg"
                    buttonSize: 30
                    iconSize: 16
                    enabled: !locationState.busy
                    toolTipText: qsTr("Close")
                    accessibleName: toolTipText
                    onClicked: root.requestDismiss()
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("%L1 selected photos").arg(
                    locationState.targetCount)
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Choose a place or click the map. Shadow stores the location non-destructively; the original EXIF remains unchanged.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            Label {
                text: qsTr("Coordinates")
                color: Theme.textSecondary
                font.weight: Font.DemiBold
            }

            TextField {
                id: latitudeField
                objectName: "locationBatchLatitudeField"
                Layout.fillWidth: true
                placeholderText: qsTr("Latitude")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                enabled: !locationState.busy
            }

            TextField {
                id: longitudeField
                objectName: "locationBatchLongitudeField"
                Layout.fillWidth: true
                placeholderText: qsTr("Longitude")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                enabled: !locationState.busy
            }

            TextField {
                id: placeField
                objectName: "locationBatchPlaceField"
                Layout.fillWidth: true
                placeholderText: qsTr("Place name (optional)")
                enabled: !locationState.busy
            }

            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("USE THESE COORDINATES")
                variant: ShadowButton.Ghost
                enabled: !locationState.busy
                    && latitudeField.text.trim().length > 0
                    && longitudeField.text.trim().length > 0
                onClicked: root.acceptManualFields()
            }

            Label {
                text: qsTr("Apply to")
                color: Theme.textSecondary
                font.weight: Font.DemiBold
            }

            ComboBox {
                id: policyBox
                objectName: "locationBatchPolicyBox"
                Layout.fillWidth: true
                enabled: !locationState.busy
                model: [
                    qsTr("Photos without a location"),
                    qsTr("All selected photos")
                ]
                onCurrentIndexChanged: {
                    locationState.mode = currentIndex === 0
                        ? "missing" : "replace"
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: previewColumn.implicitHeight + 22
                radius: Theme.controlRadius
                color: Theme.panelRaised
                border.width: 1
                border.color: locationState.previewReady
                    && Number(locationState.preview.replacementPhotoCount || 0) > 0
                    ? Theme.warningText : Theme.border

                ColumnLayout {
                    id: previewColumn
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 5

                    Label {
                        Layout.fillWidth: true
                        text: locationState.previewReady
                            ? qsTr("%L1 photos will be updated").arg(
                                  Number(locationState.preview.applicablePhotoCount || 0))
                            : qsTr("Preview the change before applying it")
                        color: Theme.textPrimary
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: locationState.previewReady
                        text: qsTr("%L1 without location · %L2 already located · %L3 skipped")
                            .arg(Number(locationState.preview.missingPhotoCount || 0))
                            .arg(Number(locationState.preview.existingPhotoCount || 0))
                            .arg(Number(locationState.preview.skippedPhotoCount || 0))
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: locationState.previewReady
                            && Number(locationState.preview.replacementPhotoCount || 0) > 0
                        text: qsTr("This will replace the current location of %L1 photos.").arg(
                            Number(locationState.preview.replacementPhotoCount || 0))
                        color: Theme.warningText
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: locationState.failed
                text: qsTr("Could not update photo locations: %1").arg(
                    locationState.errorText)
                color: Theme.errorText
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: locationState.applied
                text: qsTr("Photo locations updated")
                color: Theme.successText
                font.pixelSize: Theme.fontMeta
            }

            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.fillWidth: true

                ShadowButton {
                    Layout.fillWidth: true
                    text: locationState.previewReady
                        ? qsTr("REFRESH PREVIEW") : qsTr("PREVIEW")
                    variant: ShadowButton.Ghost
                    enabled: !locationState.busy
                        && locationState.hasCoordinate
                    onClicked: locationState.previewAssignment()
                }

                ShadowButton {
                    Layout.fillWidth: true
                    text: locationState.busy
                        ? qsTr("WORKING…") : qsTr("APPLY")
                    enabled: !locationState.busy
                        && locationState.previewReady
                        && Number(locationState.preview.applicablePhotoCount || 0) > 0
                    onClicked: locationState.applyAssignment()
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            // Qt WebView is a native child on macOS. Search remains outside
            // its rectangle so it cannot be covered by the map surface.
            LibraryMapPlaceSearch {
                id: placeSearch
                Layout.fillWidth: true
                service: root.placeSearchService
                mapController: root.mapController
                providerEligible: root.mapController.providerId === "amap"
                proposeChosenCoordinate: true
                onResultChosen: (latitude, longitude, name, label) => {
                    locationState.proposeCoordinate(
                        latitude, longitude, name,
                        "place-search:" + name)
                    latitudeField.text = Number(latitude).toLocaleString(
                        Qt.locale("C"), "f", 7)
                    longitudeField.text = Number(longitude).toLocaleString(
                        Qt.locale("C"), "f", 7)
                    placeField.text = name
                    root.synchronizeMap()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.controlRadius
                color: Theme.photoCanvas
                clip: true
                border.width: 1
                border.color: Theme.border

                LibraryWebMapSurface {
                    anchors.fill: parent
                    controller: root.mapController
                    presentationAllowed: root.visible
                        && root.nativeWebMapAllowed
                        && !placeSearch.nativeSurfaceBlocked
                }

                LibraryMapProviderOverlay {
                    anchors.fill: parent
                    workspace: root
                    providerSelected: root.mapController.providerSelected
                    providerAvailable: root.mapController.providerAvailable
                    providerRegionAvailable:
                        root.mapController.providerRegionAvailable
                    providerName: root.mapController.providerName
                    onConfigureRequested: {
                        root.configureMapRequested()
                        root.close()
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: locationState.hasCoordinate
                text: Number(locationState.latitude).toLocaleString(
                          Qt.locale("C"), "f", 6)
                    + ", "
                    + Number(locationState.longitude).toLocaleString(
                          Qt.locale("C"), "f", 6)
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    Connections {
        target: root.mapController

        function onCoordinateProposed(latitude, longitude) {
            if (!root.visible)
                return
            locationState.proposeCoordinate(
                latitude, longitude, "", "manual-map")
            latitudeField.text = Number(latitude).toLocaleString(
                Qt.locale("C"), "f", 7)
            longitudeField.text = Number(longitude).toLocaleString(
                Qt.locale("C"), "f", 7)
            placeField.clear()
            root.synchronizeMap()
        }
    }
}
