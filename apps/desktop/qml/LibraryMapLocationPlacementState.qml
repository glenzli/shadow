pragma ComponentBehavior: Bound

import QtQuick

// Owns one explicit map-to-metadata mutation lifecycle. The map supplies
// candidate coordinates; the existing metadata coordinator remains the sole
// persistence and failure-policy owner.
QtObject {
    id: placement

    required property var workspace

    property bool active: false
    property bool hasPendingCoordinate: false
    property real pendingLatitude: 0
    property real pendingLongitude: 0
    property string photoId: ""
    property string photoTitle: ""
    property bool saving: false

    readonly property bool canBegin:
        workspace.selectedPhotoId.length > 0
        && !workspace.controller.libraryMetadataBusy
    readonly property bool failed:
        active && !saving
        && workspace.controller.libraryMetadataStatusCode === "failed"
    readonly property string errorText: failed
        ? workspace.controller.libraryMetadataErrorText : ""

    signal locationSaved(string photoId, real latitude, real longitude)

    function reset() {
        if (saving)
            return false
        active = false
        hasPendingCoordinate = false
        pendingLatitude = 0
        pendingLongitude = 0
        photoId = ""
        photoTitle = ""
        return true
    }

    function begin() {
        if (!canBegin)
            return false
        photoId = String(workspace.selectedPhotoId)
        photoTitle = String(workspace.selectedTitle)
        active = true
        saving = false
        if (workspace.selectedHasCoordinates) {
            hasPendingCoordinate = true
            pendingLatitude = Number(workspace.selectedLatitude)
            pendingLongitude = Number(workspace.selectedLongitude)
        } else {
            hasPendingCoordinate = false
            pendingLatitude = 0
            pendingLongitude = 0
        }
        return true
    }

    function proposeCoordinate(latitude, longitude) {
        const nextLatitude = Number(latitude)
        const nextLongitude = Number(longitude)
        if (!active || saving || !Number.isFinite(nextLatitude)
                || !Number.isFinite(nextLongitude)
                || nextLatitude < -90 || nextLatitude > 90
                || nextLongitude < -180 || nextLongitude > 180)
            return false
        pendingLatitude = nextLatitude
        pendingLongitude = nextLongitude
        hasPendingCoordinate = true
        return true
    }

    function commit() {
        if (!active || saving || !hasPendingCoordinate
                || workspace.controller.libraryMetadataBusy
                || String(workspace.selectedPhotoId) !== photoId)
            return false
        saving = true
        workspace.controller.setLibraryCoordinates(
            photoId, "set", pendingLatitude, pendingLongitude, "")
        if (!workspace.controller.libraryMetadataBusy
                && workspace.controller.libraryMetadataStatusCode
                    !== "saving") {
            saving = false
            return false
        }
        return true
    }

    property Connections controllerConnections: Connections {
        target: placement.workspace.controller

        function onLibraryMetadataChanged() {
            if (!placement.saving)
                return
            const status =
                placement.workspace.controller.libraryMetadataStatusCode
            if (status === "failed") {
                placement.saving = false
                return
            }
            if (status !== "saved")
                return
            const savedPhotoId = placement.photoId
            const savedLatitude = placement.pendingLatitude
            const savedLongitude = placement.pendingLongitude
            placement.saving = false
            placement.reset()
            placement.locationSaved(
                savedPhotoId, savedLatitude, savedLongitude)
        }
    }

    property Connections workspaceConnections: Connections {
        target: placement.workspace

        function onSelectedPhotoIdChanged() {
            if (placement.active && !placement.saving
                    && String(placement.workspace.selectedPhotoId)
                        !== placement.photoId)
                placement.reset()
        }
    }
}
