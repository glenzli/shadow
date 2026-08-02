pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtLocation
import QtPositioning
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    required property var workspace

    readonly property bool googleProviderSelected: workspace.mapProviderPreferences.libraryMapProvider === "google"
    readonly property bool googleProviderAvailable: workspace.mapProviderPreferences.googleApiKeyStored && workspace.mapProviderPreferences.googleMapTilesAllowed

    color: Theme.window
    focus: true

    LibraryMapLocationPlacementState {
        id: locationPlacement
        workspace: root.workspace
        onLocationSaved: root.scheduleViewport()
    }

    function requestViewport() {
        if (!visible || map.width < 2 || map.height < 2)
            return;
        const topLeft = map.toCoordinate(Qt.point(0, 0), false);
        const bottomRight = map.toCoordinate(Qt.point(map.width, map.height), false);
        if (!topLeft.isValid || !bottomRight.isValid)
            return;
        let west = Number(topLeft.longitude);
        let east = Number(bottomRight.longitude);
        if (Math.abs(west - east) < 0.000001) {
            west = -180;
            east = 180;
        }
        const columns = Math.max(1, Math.min(64, Math.ceil(map.width / 76)));
        const rows = Math.max(1, Math.min(48, Math.ceil(map.height / 76)));
        root.workspace.controller.requestLibraryMapViewport(Number(bottomRight.latitude), west, Number(topLeft.latitude), east, columns, rows);
        if (root.googleProviderSelected && root.workspace.googleMapTilesService.active) {
            root.workspace.googleMapTilesService.updateViewport(Number(topLeft.latitude), Number(bottomRight.latitude), east, west, Math.floor(map.zoomLevel));
        }
    }

    function scheduleViewport() {
        viewportTimer.restart();
    }

    function activateCluster(cluster) {
        if (locationPlacement.active)
            return;
        if (Number(cluster.photoCount) > 1) {
            map.center = QtPositioning.coordinate(Number(cluster.latitude), Number(cluster.longitude));
            map.zoomLevel = Math.min(map.maximumZoomLevel, map.zoomLevel + 2);
            return;
        }
        root.workspace.selectMapPhoto(cluster);
    }

    function forceGalleryFocus() {
        map.forceActiveFocus();
    }

    function synchronizeGoogleRuntime() {
        const service = root.workspace.googleMapTilesService;
        service.language = root.workspace.preferences.effectiveLanguage === "zh_CN" ? "zh-CN" : "en-US";
        service.active = root.visible && root.googleProviderSelected && root.googleProviderAvailable;
        if (service.active)
            root.scheduleViewport();
    }

    function googleStatusMessage() {
        switch (String(root.workspace.googleMapTilesService.statusCode)) {
        case "permission-required":
            return qsTr("Allow Google 2D map tiles in Map & Location Services.");
        case "credential-unavailable":
        case "forbidden":
            return qsTr("Google Maps authorization failed. Check the API key and its API restrictions.");
        case "quota-exceeded":
            return qsTr("The Google map tile quota has been reached.");
        case "rate-limited":
            return qsTr("Google Maps is limiting requests. Shadow will retry gradually.");
        case "provider-unavailable":
        case "network-error":
            return qsTr("Google Maps is temporarily unavailable.");
        case "invalid-session-json":
        case "invalid-session-contract":
        case "invalid-viewport-json":
        case "invalid-viewport-contract":
        case "invalid-tile-image":
        case "bad-request":
        case "invalid":
        case "required":
        case "not-found":
            return qsTr("Google Maps returned an unsupported response.");
        default:
            return "";
        }
    }

    Plugin {
        id: itemOverlayPlugin
        name: "itemsoverlay"
    }

    Timer {
        id: viewportTimer
        interval: 180
        repeat: false
        onTriggered: root.requestViewport()
    }

    GoogleMapTileLayer {
        id: googleBaseMap
        anchors.fill: parent
        z: 0
        visible: root.googleProviderSelected
        active: visible && root.visible
        service: root.workspace.googleMapTilesService
        centerLatitude: Number(map.center.latitude)
        centerLongitude: Number(map.center.longitude)
        zoomLevel: map.zoomLevel
    }

    Map {
        id: map
        objectName: "libraryMap"
        anchors.fill: parent
        z: 1
        plugin: itemOverlayPlugin
        center: QtPositioning.coordinate(20, 0)
        zoomLevel: 2.5
        copyrightsVisible: false

        onCenterChanged: root.scheduleViewport()
        onZoomLevelChanged: root.scheduleViewport()
        onWidthChanged: root.scheduleViewport()
        onHeightChanged: root.scheduleViewport()
        TapHandler {
            enabled: locationPlacement.active && !locationPlacement.saving
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: eventPoint => {
                const coordinate = map.toCoordinate(eventPoint.position, false);
                if (coordinate.isValid)
                    locationPlacement.proposeCoordinate(Number(coordinate.latitude), Number(coordinate.longitude));
            }
        }

        HoverHandler {
            enabled: locationPlacement.active
            cursorShape: Qt.CrossCursor
        }

        MapItemView {
            model: root.workspace.controller.libraryMapClusters

            delegate: MapQuickItem {
                id: marker
                required property var modelData

                coordinate: QtPositioning.coordinate(Number(modelData.latitude), Number(modelData.longitude))
                anchorPoint.x: markerBubble.width / 2
                anchorPoint.y: markerBubble.height / 2

                sourceItem: Rectangle {
                    id: markerBubble
                    implicitWidth: Math.max(30, markerLabel.implicitWidth + 14)
                    implicitHeight: 30
                    radius: 15
                    color: marker.modelData.photoCount > 1 ? Theme.accent : Theme.panelRaised
                    border.width: 1
                    border.color: marker.modelData.photoCount > 1 ? Theme.accentPressed : Theme.accent

                    Label {
                        id: markerLabel
                        anchors.centerIn: parent
                        text: Number(marker.modelData.photoCount) > 1 ? Number(marker.modelData.photoCount).toLocaleString(Qt.locale()) : "•"
                        color: marker.modelData.photoCount > 1 ? Theme.accentForeground : Theme.accent
                        font.pixelSize: Number(marker.modelData.photoCount) > 1 ? 11 : 18
                        font.weight: Font.DemiBold
                    }

                    TapHandler {
                        enabled: !locationPlacement.active
                        onTapped: root.activateCluster(marker.modelData)
                    }

                    ToolTip {
                        visible: markerHover.hovered
                        text: Number(marker.modelData.photoCount) > 1 ? qsTr("%L1 photos").arg(Number(marker.modelData.photoCount)) : String(marker.modelData.title)
                    }

                    HoverHandler {
                        id: markerHover
                    }
                }
            }
        }

        MapQuickItem {
            visible: locationPlacement.active && locationPlacement.hasPendingCoordinate
            coordinate: QtPositioning.coordinate(locationPlacement.pendingLatitude, locationPlacement.pendingLongitude)
            anchorPoint.x: pendingPin.width / 2
            anchorPoint.y: pendingPin.height

            sourceItem: Rectangle {
                id: pendingPin
                width: 38
                height: 38
                radius: 19
                color: Theme.accent
                border.width: 2
                border.color: Theme.accentForeground

                ShadowIcon {
                    anchors.centerIn: parent
                    width: 22
                    height: 22
                    source: "qrc:/icons/location-pin.svg"
                    color: Theme.accentForeground
                }
            }
        }
    }

    LibraryMapProviderOverlay {
        anchors.fill: parent
        z: 5
        workspace: root.workspace
        googleProviderSelected: root.googleProviderSelected
        googleProviderAvailable: root.googleProviderAvailable
        googleStatusMessage: root.googleStatusMessage()
        onConfigureRequested: root.workspace.openMapProviderSettingsRequested()
    }

    Rectangle {
        id: placementPanel
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 14
        width: locationPlacement.active ? Math.min(360, root.width - 28) : 38
        height: locationPlacement.active ? placementContent.implicitHeight + 24 : 38
        radius: 8
        color: Theme.panelRaised
        border.width: 1
        border.color: locationPlacement.active ? Theme.accent : Theme.border
        z: 6

        ShadowIconButton {
            anchors.centerIn: parent
            visible: !locationPlacement.active
            source: "qrc:/icons/location-pin.svg"
            enabled: locationPlacement.canBegin
            toolTipText: enabled ? qsTr("Set the selected photo location on the map") : qsTr("Select a photo before setting its location")
            accessibleName: toolTipText
            onClicked: locationPlacement.begin()
        }

        ColumnLayout {
            id: placementContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            visible: locationPlacement.active
            spacing: 7

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ShadowIcon {
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    source: "qrc:/icons/location-pin.svg"
                    color: Theme.accent
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("SET PHOTO LOCATION")
                    color: Theme.textPrimary
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.7
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
                text: locationPlacement.photoTitle
                color: Theme.textSecondary
                font.pixelSize: 11
                elide: Text.ElideMiddle
            }

            Label {
                Layout.fillWidth: true
                text: locationPlacement.hasPendingCoordinate ? qsTr("Click elsewhere to move the pin, then confirm.") : qsTr("Click the map where this photo was taken.")
                color: Theme.textMuted
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    visible: locationPlacement.hasPendingCoordinate
                    text: Number(locationPlacement.pendingLatitude).toLocaleString(Qt.locale("C"), "f", 6) + ", " + Number(locationPlacement.pendingLongitude).toLocaleString(Qt.locale("C"), "f", 6)
                    color: Theme.textPrimary
                    font.pixelSize: 10
                }

                Item {
                    Layout.fillWidth: !locationPlacement.hasPendingCoordinate
                }

                BusyIndicator {
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    visible: locationPlacement.saving
                    running: visible
                }

                ShadowIconButton {
                    visible: !locationPlacement.saving
                    source: "qrc:/icons/check.svg"
                    enabled: locationPlacement.hasPendingCoordinate && !root.workspace.controller.libraryMetadataBusy
                    toolTipText: qsTr("Save this photo location")
                    accessibleName: toolTipText
                    onClicked: locationPlacement.commit()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: locationPlacement.failed
                text: qsTr("Could not save the map location: %1").arg(locationPlacement.errorText)
                color: Theme.errorText
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }
        }
    }

    Label {
        anchors.centerIn: parent
        width: Math.min(420, parent.width - 48)
        visible: !root.workspace.controller.libraryMapBusy && (root.workspace.controller.libraryMapFailed || root.workspace.controller.libraryMapClusters.length === 0)
        text: root.workspace.controller.libraryMapFailed ? qsTr("Map locations could not be loaded.") : qsTr("No geotagged photos in this map area.")
        color: Theme.textSecondary
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
    }

    Connections {
        target: root.workspace.controller

        function onFiltersChanged() {
            root.scheduleViewport();
        }

        function onLibraryMetadataChanged() {
            root.scheduleViewport();
        }
    }

    Connections {
        target: root.workspace.mapProviderPreferences

        function onLibraryMapProviderChanged() {
            root.synchronizeGoogleRuntime();
        }
    }

    Connections {
        target: root.workspace.preferences

        function onEffectiveLanguageChanged() {
            root.synchronizeGoogleRuntime();
        }
    }

    onVisibleChanged: synchronizeGoogleRuntime()

    Component.onCompleted: {
        root.synchronizeGoogleRuntime();
        root.scheduleViewport();
    }
    Component.onDestruction: root.workspace.googleMapTilesService.active = false
}
