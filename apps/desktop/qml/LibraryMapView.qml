pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the map canvas inside the application-level Map workspace. Library
// scope selection belongs to LibraryMapWorkspace so map navigation never
// mutates the current album/filter context implicitly.
Rectangle {
    id: root

    required property var workspace

    color: Theme.window
    focus: true

    function requestViewport(south, west, north, east, zoom) {
        const columns = Math.max(1, Math.min(64, Math.ceil(mapRegion.width / 76)))
        const rows = Math.max(1, Math.min(48, Math.ceil(mapRegion.height / 76)))
        root.workspace.controller.requestLibraryMapViewport(
            Number(south), Number(west), Number(north), Number(east),
            columns, rows)
    }

    function refreshClusters() {
        root.workspace.libraryWebMapController.setClusters(
            root.workspace.controller.libraryMapClusters)
    }

    function synchronizePlacement() {
        root.workspace.libraryWebMapController.setPlacementActive(
            locationPlacement.active)
        root.workspace.libraryWebMapController.setPendingCoordinate(
            locationPlacement.hasPendingCoordinate,
            locationPlacement.pendingLatitude,
            locationPlacement.pendingLongitude)
    }

    function forceGalleryFocus() {
        mapRegion.forceActiveFocus()
    }

    function beginProviderContext() {
        const controller = root.workspace.libraryWebMapController
        if (root.workspace.selectedHasCoordinates) {
            controller.beginMapContext(
                Number(root.workspace.selectedLatitude),
                Number(root.workspace.selectedLongitude))
        } else {
            controller.beginMapContext(
                Number(controller.centerLatitude),
                Number(controller.centerLongitude))
        }
    }

    LibraryMapLocationPlacementState {
        id: locationPlacement
        workspace: root.workspace
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: mapToolbar.implicitHeight + 16
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.border

            RowLayout {
                id: mapToolbar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 8
                spacing: 10

                LibraryMapPlaceSearch {
                    id: placeSearch
                    Layout.preferredWidth: Math.min(390, Math.max(250, root.width * 0.36))
                    service: root.workspace.amapPlaceSearchService
                    mapController: root.workspace.libraryWebMapController
                    providerEligible:
                        root.workspace.libraryWebMapController.providerId === "amap"
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    Label {
                        Layout.fillWidth: true
                        text: root.workspace.libraryWebMapController.providerSelected
                            ? root.workspace.libraryWebMapController.providerPolicy === "auto"
                                ? qsTr("Auto · %1").arg(
                                    root.workspace.libraryWebMapController.providerName)
                                : root.workspace.libraryWebMapController.providerName
                            : qsTr("Basemap unavailable")
                        color: root.workspace.libraryWebMapController.providerSelected
                            ? Theme.textPrimary : Theme.textMuted
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: !root.workspace.libraryWebMapController.providerRegionAvailable
                            ? qsTr("This provider does not cover the current map region.")
                            : root.workspace.libraryWebMapController.statusCode.length > 0
                            ? qsTr("The map service is unavailable. Check the provider credentials and network connection.")
                            : root.workspace.controller.libraryMapFailed
                                ? qsTr("Map locations could not be loaded.")
                                : root.workspace.controller.libraryMapClusters.length === 0
                                    ? qsTr("No geotagged photos in this map area.")
                                    : qsTr("%L1 photos in view").arg(
                                          root.workspace.controller.libraryMapPhotoCount)
                        color: !root.workspace.libraryWebMapController.providerRegionAvailable
                            || root.workspace.libraryWebMapController.statusCode.length > 0
                            ? Theme.errorText : Theme.textSecondary
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                }

                BusyIndicator {
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                    visible: root.workspace.controller.libraryMapBusy
                        || root.workspace.libraryWebMapController.busy
                    running: visible
                }

                Rectangle {
                    id: placementPanel
                    Layout.preferredWidth: locationPlacement.active ? 350 : 38
                    Layout.preferredHeight: locationPlacement.active
                        ? placementContent.implicitHeight + 16 : 38
                    radius: 8
                    color: Theme.panelRaised
                    border.width: 1
                    border.color: locationPlacement.active ? Theme.accent : Theme.border

                    ShadowIconButton {
                        anchors.centerIn: parent
                        visible: !locationPlacement.active
                        source: "qrc:/icons/location-pin.svg"
                        enabled: locationPlacement.canBegin
                            && root.workspace.libraryWebMapController.providerAvailable
                        toolTipText: enabled
                            ? qsTr("Set the selected photo location on the map")
                            : qsTr("Select a photo and configure a map before setting its location")
                        accessibleName: toolTipText
                        onClicked: locationPlacement.begin()
                    }

                    ColumnLayout {
                        id: placementContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8
                        visible: locationPlacement.active
                        spacing: 5

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            ShadowIcon {
                                Layout.preferredWidth: 18
                                Layout.preferredHeight: 18
                                source: "qrc:/icons/location-pin.svg"
                                color: Theme.accent
                            }

                            Label {
                                Layout.fillWidth: true
                                text: locationPlacement.hasPendingCoordinate
                                    ? Number(locationPlacement.pendingLatitude).toLocaleString(
                                          Qt.locale("C"), "f", 6) + ", "
                                      + Number(locationPlacement.pendingLongitude).toLocaleString(
                                          Qt.locale("C"), "f", 6)
                                    : qsTr("Click the map where this photo was taken.")
                                color: Theme.textSecondary
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }

                            BusyIndicator {
                                Layout.preferredWidth: 22
                                Layout.preferredHeight: 22
                                visible: locationPlacement.saving
                                running: visible
                            }

                            ShadowIconButton {
                                visible: !locationPlacement.saving
                                source: "qrc:/icons/check.svg"
                                enabled: locationPlacement.hasPendingCoordinate
                                    && !root.workspace.controller.libraryMetadataBusy
                                toolTipText: qsTr("Save this photo location")
                                accessibleName: toolTipText
                                onClicked: locationPlacement.commit()
                            }

                            ShadowIconButton {
                                source: "qrc:/icons/close.svg"
                                enabled: !locationPlacement.saving
                                toolTipText: qsTr("Cancel map location placement")
                                accessibleName: toolTipText
                                onClicked: locationPlacement.reset()
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: locationPlacement.failed
                            text: qsTr("Could not save the map location: %1").arg(
                                locationPlacement.errorText)
                            color: Theme.errorText
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }

        Item {
            id: mapRegion
            Layout.fillWidth: true
            Layout.fillHeight: true
            focus: true

            LibraryWebMapSurface {
                anchors.fill: parent
                controller: root.workspace.libraryWebMapController
                presentationAllowed: root.visible
                    && root.workspace.nativeWebMapAllowed
                    && !placeSearch.nativeSurfaceBlocked
            }

            LibraryMapProviderOverlay {
                anchors.fill: parent
                workspace: root.workspace
                providerSelected:
                    root.workspace.libraryWebMapController.providerSelected
                providerAvailable:
                    root.workspace.libraryWebMapController.providerAvailable
                providerRegionAvailable:
                    root.workspace.libraryWebMapController.providerRegionAvailable
                providerName: root.workspace.libraryWebMapController.providerName
                onConfigureRequested:
                    root.workspace.openMapProviderSettingsRequested()
            }

        }
    }

    Connections {
        target: root.workspace.libraryWebMapController
        enabled: root.workspace.nativeWebMapAllowed

        function onViewportChanged(south, west, north, east, zoom) {
            root.requestViewport(south, west, north, east, zoom)
        }

        function onClusterActivated(cluster) {
            root.workspace.selectMapPhoto(cluster)
        }

        function onCoordinateProposed(latitude, longitude) {
            locationPlacement.proposeCoordinate(latitude, longitude)
        }
    }

    Connections {
        target: locationPlacement
        function onActiveChanged() { root.synchronizePlacement() }
        function onHasPendingCoordinateChanged() { root.synchronizePlacement() }
        function onPendingLatitudeChanged() { root.synchronizePlacement() }
        function onPendingLongitudeChanged() { root.synchronizePlacement() }
    }

    Connections {
        target: root.workspace.controller

        function onLibraryMapChanged() { root.refreshClusters() }
        function onFiltersChanged() {
            root.workspace.libraryWebMapController.setCenter(
                root.workspace.libraryWebMapController.centerLatitude,
                root.workspace.libraryWebMapController.centerLongitude,
                root.workspace.libraryWebMapController.zoomLevel)
        }
    }

    Connections {
        target: root.workspace.preferences
        function onEffectiveLanguageChanged() {
            root.workspace.libraryWebMapController.setLanguage(
                root.workspace.preferences.effectiveLanguage)
        }
    }

    onVisibleChanged: {
        if (visible) {
            root.beginProviderContext()
            root.workspace.libraryWebMapController.active =
                root.workspace.nativeWebMapAllowed
            root.refreshClusters()
        } else {
            root.workspace.libraryWebMapController.active = false
        }
    }

    Connections {
        target: root.workspace

        function onNativeWebMapAllowedChanged() {
            if (!root.visible)
                return
            if (root.workspace.nativeWebMapAllowed) {
                root.beginProviderContext()
                root.refreshClusters()
                root.workspace.libraryWebMapController.active = true
            } else {
                root.workspace.libraryWebMapController.active = false
            }
        }
    }

    Component.onCompleted: {
        root.workspace.libraryWebMapController.setLanguage(
            root.workspace.preferences.effectiveLanguage)
        if (root.visible) root.beginProviderContext()
        root.workspace.libraryWebMapController.active = root.visible
            && root.workspace.nativeWebMapAllowed
        root.refreshClusters()
        root.synchronizePlacement()
    }

    Component.onDestruction:
        root.workspace.libraryWebMapController.active = false
}
