pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick

// Owns the selection-wide map-placement and preview/apply lifecycle. The
// backend preview remains authoritative for conflict counts and mutation scope.
QtObject {
    id: state

    required property var controller
    property var targets: []
    property bool open: false
    property bool hasCoordinate: false
    property real latitude: 0
    property real longitude: 0
    property string placeName: ""
    property string sourceLabel: "manual-map"
    property string mode: "missing"
    property bool applying: false
    property bool applied: false

    readonly property var preview: controller.libraryCoordinateBatchPreview
    readonly property var receipt: controller.libraryMetadataBatchReceipt
    readonly property bool busy: controller.libraryMetadataBusy
    readonly property bool previewMatchesInput:
        String(preview.mode || "") === mode
        && Math.abs(Number(preview.latitude || 0) - latitude) < 0.0000001
        && Math.abs(Number(preview.longitude || 0) - longitude) < 0.0000001
        && String(preview.placeName || "") === placeName.trim()
    readonly property bool previewReady:
        String(preview.previewId || "").length > 0
        && previewMatchesInput
    readonly property bool failed:
        open && !busy && controller.libraryMetadataStatusCode === "failed"
    readonly property string errorText: failed
        ? controller.libraryMetadataErrorText : ""
    readonly property int targetCount: targets.length

    signal locationApplied(int appliedPhotoCount)

    function present(selectedTargets, initialHasCoordinate,
                     initialLatitude, initialLongitude, initialPlaceName) {
        controller.clearLibraryMetadata()
        targets = selectedTargets || []
        open = targets.length > 0
        hasCoordinate = Boolean(initialHasCoordinate)
        latitude = hasCoordinate ? Number(initialLatitude) : 0
        longitude = hasCoordinate ? Number(initialLongitude) : 0
        placeName = hasCoordinate ? String(initialPlaceName || "") : ""
        sourceLabel = "manual-map"
        mode = "missing"
        applying = false
        applied = false
        return open
    }

    function close() {
        if (busy)
            return false
        open = false
        applying = false
        return true
    }

    function proposeCoordinate(nextLatitude, nextLongitude,
                               nextPlaceName, nextSourceLabel) {
        const lat = Number(nextLatitude)
        const lon = Number(nextLongitude)
        if (!open || busy || !Number.isFinite(lat) || !Number.isFinite(lon)
                || lat < -90 || lat > 90 || lon < -180 || lon > 180)
            return false
        hasCoordinate = true
        latitude = lat
        longitude = lon
        placeName = String(nextPlaceName || "")
        sourceLabel = String(nextSourceLabel || "manual-map")
        return true
    }

    function previewAssignment() {
        if (!open || busy || !hasCoordinate || targets.length === 0)
            return false
        applied = false
        controller.previewLibraryCoordinateBatch(
            targets, mode, latitude, longitude, placeName, sourceLabel)
        return controller.libraryMetadataBusy
            || controller.libraryMetadataStatusCode === "ready"
    }

    function applyAssignment() {
        const previewId = String(preview.previewId || "")
        if (!open || busy || previewId.length === 0)
            return false
        applying = true
        controller.applyLibraryCoordinateBatch(previewId)
        return controller.libraryMetadataBusy
            || controller.libraryMetadataStatusCode === "applied"
    }

    property Connections controllerConnections: Connections {
        target: state.controller

        function onLibraryMetadataChanged() {
            if (!state.open || !state.applying)
                return
            const status = state.controller.libraryMetadataStatusCode
            if (status === "failed") {
                state.applying = false
                return
            }
            if (status !== "applied")
                return
            state.applying = false
            state.applied = true
            state.locationApplied(
                Number(state.receipt.appliedPhotoCount || 0))
        }
    }
}
