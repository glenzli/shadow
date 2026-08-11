pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: review
    objectName: "reviewWorkspace"

    required property var controller
    required property var justifiedReviewLayout
    required property var reviewGalleryGrouping
    required property var preferences
    required property var mapProviderPreferences
    required property var libraryWebMapController
    required property var amapPlaceSearchService
    required property var personalProfile
    required property var semanticSearchController
    required property var smartCategoryController
    required property var imageUnderstandingController

    ReviewSelectionState {
        id: selectionState
        controller: review.controller
        focusDetailEnabled: review.galleryPresentation
            === ReviewWorkspace.SinglePhotoFilmstrip
        onPrimaryContextInvalidated: review.precisionOpenStatus = ""
    }

    ReviewComparisonState {
        id: comparisonState
        controller: review.controller
        selection: selectionState
        navigationModel: review.justifiedReviewLayout
    }

    ReviewCullingState {
        id: cullingState
        selection: selectionState
    }

    readonly property alias comparison: comparisonState
    readonly property alias culling: cullingState
    readonly property alias selectedPhotoId: selectionState.selectedPhotoId
    readonly property alias selectedRepresentationId:
        selectionState.selectedRepresentationId
    readonly property alias selectedVisualHandle: selectionState.selectedVisualHandle
    readonly property alias selectedDecisionHeadSequence:
        selectionState.selectedDecisionHeadSequence
    readonly property alias selectedDecisionFlag: selectionState.selectedDecisionFlag
    readonly property alias selectedDecisionRating: selectionState.selectedDecisionRating
    readonly property alias selectedLiked: selectionState.selectedLiked
    readonly property alias selectedColorLabel: selectionState.selectedColorLabel
    readonly property alias selectedTitle: selectionState.selectedTitle
    readonly property alias selectedLocationId: selectionState.selectedLocationId
    readonly property alias selectedPath: selectionState.selectedPath
    readonly property alias selectedSourceAvailable:
        selectionState.selectedSourceAvailable
    readonly property alias selectedIsRemote: selectionState.selectedIsRemote
    readonly property alias selectedRemoteOriginalCached:
        selectionState.selectedRemoteOriginalCached
    readonly property alias selectedRemoteConnectionId:
        selectionState.selectedRemoteConnectionId
    readonly property alias selectedRemotePreviewUnavailableReason:
        selectionState.selectedRemotePreviewUnavailableReason
    readonly property alias selectedRole: selectionState.selectedRole
    readonly property alias selectedVisualSource: selectionState.selectedVisualSource
    readonly property alias selectedWidth: selectionState.selectedWidth
    readonly property alias selectedHeight: selectionState.selectedHeight

    function remoteLibraryConnectionStatus(connectionId) {
        const identity = String(connectionId || "")
        const connections = controller.remoteLibraries || []
        for (let index = 0; index < connections.length; ++index) {
            if (String(connections[index].id || "") === identity)
                return String(connections[index].statusCode || "")
        }
        return ""
    }

    function remoteLibraryConnectionOffline(connectionId) {
        const status = remoteLibraryConnectionStatus(connectionId)
        return status.length === 0 || status === "offline-ready"
            || status === "sync-failed"
    }
    readonly property alias selectedHasMetadata: selectionState.selectedHasMetadata
    readonly property alias selectedCameraMake: selectionState.selectedCameraMake
    readonly property alias selectedCameraModel: selectionState.selectedCameraModel
    readonly property alias selectedLensMake: selectionState.selectedLensMake
    readonly property alias selectedLensModel: selectionState.selectedLensModel
    readonly property alias selectedCapturedAtUnixSeconds:
        selectionState.selectedCapturedAtUnixSeconds
    readonly property alias selectedHasCoordinates:
        selectionState.selectedHasCoordinates
    readonly property alias selectedLatitude: selectionState.selectedLatitude
    readonly property alias selectedLongitude: selectionState.selectedLongitude
    readonly property alias selectedPlaceName: selectionState.selectedPlaceName
    readonly property alias selectedResolvedPlaceName:
        selectionState.selectedResolvedPlaceName
    readonly property alias selectedIsoSpeed: selectionState.selectedIsoSpeed
    readonly property alias selectedExposureTimeSeconds:
        selectionState.selectedExposureTimeSeconds
    readonly property alias selectedApertureFNumber:
        selectionState.selectedApertureFNumber
    readonly property alias selectedFocalLengthMm: selectionState.selectedFocalLengthMm
    readonly property alias selectedFocalLength35mm:
        selectionState.selectedFocalLength35mm
    readonly property alias selectedRawWidth: selectionState.selectedRawWidth
    readonly property alias selectedRawHeight: selectionState.selectedRawHeight
    readonly property alias selectedSensorBits: selectionState.selectedSensorBits
    readonly property alias selectedCfaPattern: selectionState.selectedCfaPattern
    readonly property alias selectedDngVersion: selectionState.selectedDngVersion
    readonly property alias selectedHasFocusObservation:
        selectionState.selectedHasFocusObservation
    readonly property alias selectedFocusObservationSchemaVersion:
        selectionState.selectedFocusObservationSchemaVersion
    readonly property alias selectedFocusObservationSource:
        selectionState.selectedFocusObservationSource
    readonly property alias selectedFocusObservationCenterX:
        selectionState.selectedFocusObservationCenterX
    readonly property alias selectedFocusObservationCenterY:
        selectionState.selectedFocusObservationCenterY
    readonly property alias selectedFocusObservationWidth:
        selectionState.selectedFocusObservationWidth
    readonly property alias selectedFocusObservationHeight:
        selectionState.selectedFocusObservationHeight
    readonly property alias selectedFocusObservationConfirmed:
        selectionState.selectedFocusObservationConfirmed
    readonly property alias selectedFocusObservationConfidence:
        selectionState.selectedFocusObservationConfidence
    readonly property alias selectedHasTechnicalObservation:
        selectionState.selectedHasTechnicalObservation
    readonly property alias selectedTechnicalInputWidth:
        selectionState.selectedTechnicalInputWidth
    readonly property alias selectedTechnicalInputHeight:
        selectionState.selectedTechnicalInputHeight
    readonly property alias selectedTechnicalPreprocessingVersion:
        selectionState.selectedTechnicalPreprocessingVersion
    readonly property alias selectedTechnicalImplementationVersion:
        selectionState.selectedTechnicalImplementationVersion
    readonly property alias selectedMeanLuma: selectionState.selectedMeanLuma
    readonly property alias selectedP01Luma: selectionState.selectedP01Luma
    readonly property alias selectedP50Luma: selectionState.selectedP50Luma
    readonly property alias selectedP99Luma: selectionState.selectedP99Luma
    readonly property alias selectedNearBlackFraction:
        selectionState.selectedNearBlackFraction
    readonly property alias selectedNearWhiteFraction:
        selectionState.selectedNearWhiteFraction
    readonly property alias selectedLaplacianVariance:
        selectionState.selectedLaplacianVariance
    readonly property alias selectedEdgeEnergy: selectionState.selectedEdgeEnergy
    readonly property alias selectedPhotoCount: selectionState.selectedPhotoCount
    // The grid stays the broad library browser. The single presentation is a
    // deliberately focused culling surface that still consumes the same
    // filtered catalogue model.
    enum GalleryPresentation {
        JustifiedGrid,
        SinglePhotoFilmstrip,
        Map
    }
    property int galleryPresentation: ReviewWorkspace.JustifiedGrid
    property string precisionOpenStatus: ""

    readonly property bool canMutateDecision: selectedPhotoId.length > 0
        && !comparison.compareMode
        && !culling.arenaActive
        && !controller.scanning && !controller.refreshing
        && !controller.busy && !controller.loadingMore
        && !controller.comparisonBusy && !controller.decisionBusy
    // Editing consumes the immutable source identity, not the Library query or
    // generated thumbnail. A row already published by an in-flight import is
    // therefore admissible; EditController owns any later open serialization.
    readonly property bool canOpenSelectedPhoto: selectedPhotoId.length > 0
        && selectedRepresentationId.length > 0
        && (selectedIsRemote || selectedPath.length > 0)
        && !comparison.compareMode && !culling.arenaActive
        && !controller.remoteLibraryMaterializing
    readonly property var currentLibraryAlbum: {
        const albums = controller.libraryAlbums
        const selectedId = String(controller.libraryAlbumId)
        for (let index = 0; index < albums.length; ++index) {
            if (String(albums[index].id) === selectedId)
                return albums[index]
        }
        return null
    }
    readonly property string currentLibraryAlbumName:
        currentLibraryAlbum === null ? "" : String(currentLibraryAlbum.name)
    readonly property bool currentLibraryAlbumIsManual:
        currentLibraryAlbum !== null
            && String(currentLibraryAlbum.kind) === "manual"
    readonly property bool hasLibraryFacetFilter:
        controller.filterCaptureMonth.length > 0
        || controller.filterChineseLunarMonth > 0
        || controller.filterChineseLunarDay > 0
        || controller.filterChineseLunarMonthType !== "all"
        || controller.filterCameraKey.length > 0
        || controller.filterLensKey.length > 0
        || controller.filterCountryKey.length > 0
        || controller.filterLocalityKey.length > 0
        || controller.travelFilterEnabled
        || controller.dailyFilterEnabled
    readonly property bool hasLibraryKeywordFilter:
        controller.filterKeywordIdsAll.length > 0
        || controller.filterExcludedKeywordIdsAny.length > 0
    readonly property bool hasActiveLibraryFilter:
        controller.filterFlag !== "all"
        || controller.filterMinimumRating > 0
        || controller.filterColorLabel !== "all"
        || controller.filterEditState !== "all"
        || controller.filterLiked !== "all"
        || controller.filterExcludedFlag !== "all"
        || controller.filterExcludedColorLabel !== "all"
        || hasLibraryFacetFilter
        || hasLibraryKeywordFilter
        || semanticSearchController.hasResults
        || smartCategoryController.selectedCategoryId.length > 0
    readonly property var manualLibraryAlbums: {
        const albums = controller.libraryAlbums
        const manualAlbums = []
        for (let index = 0; index < albums.length; ++index) {
            if (String(albums[index].kind) === "manual")
                manualAlbums.push(albums[index])
        }
        return manualAlbums
    }
    signal openPrecisionRequested(string photoId, string representationId,
                                  string sourcePath, string photoTitle,
                                  string previewSource)
    signal openLibraryManagementRequested()
    signal openMapProviderSettingsRequested()
    signal exportRequested(var targets)

    ReviewMetadataPresentation {
        id: metadataPresentation
        workspace: review
    }

    MetadataWindow {
        id: metadataWindow
        transientParent: review.Window.window
        preferences: review.preferences
        controller: review.controller
        photoTitle: review.selectedTitle
        photoId: review.selectedPhotoId
        selectionTargets: review.batchSelectionTargets()
        sourcePath: review.selectedPath
        hasMetadata: review.selectedHasMetadata
        metadataPending: review.controller.photoInspectionBusy
        metadataFailed: review.controller.photoInspectionFailed
        fields: metadataPresentation.metadataFields()
        onRetryRequested: review.controller.retryPhotoInspection()
    }

    LibraryAlbumDialogs {
        id: albumDialogs
        anchors.fill: parent
        controller: review.controller
        hasActiveLibraryFilter: review.hasActiveLibraryFilter
        manualAlbums: review.manualLibraryAlbums
    }

    LibraryKeywordPopup {
        id: keywordPopup
        workspace: review
    }

    ReviewSharedGradePicker {
        id: sharedGradePicker
        workspace: review
    }

    LibraryMissingPhotoDialogs {
        id: missingPhotoDialogs
        anchors.fill: parent
        controller: review.controller
    }

    readonly property color panel: Theme.panel
    readonly property color panelRaised: Theme.panelRaised
    readonly property color border: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textSecondary: Theme.textSecondary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent

    Component.onCompleted: {
        justifiedReviewLayout.targetRowHeight = preferences.libraryThumbnailScale
    }

    function selectionKey(photoId, representationId) {
        return selectionState.selectionKey(photoId, representationId)
    }

    function isPhotoSelected(photoId, representationId) {
        return selectionState.isPhotoSelected(photoId, representationId)
    }

    function batchSelectionTargets() {
        return selectionState.batchSelectionTargets()
    }

    function snapshotForCard(card) {
        return selectionState.snapshotForCard(card)
    }

    function selectedVisualSnapshot() {
        return selectionState.selectedSnapshot()
    }

    function compareSelectedPhotos() {
        if (selectedPhotoCount !== 2 || comparison.compareMode
                || culling.arenaActive)
            return false
        const snapshots = selectionState.selectedVisualSnapshots()
        if (snapshots.length !== 2)
            return false
        return comparison.startSelectedComparison(snapshots[0], snapshots[1])
    }

    function toggleSelectedCandidate() {
        return cullingState.toggleSelectedCandidate()
    }

    function selectionContainsRemote() {
        const targets = batchSelectionTargets()
        for (let index = 0; index < targets.length; ++index) {
            if (Boolean(targets[index].isRemote))
                return true
        }
        return false
    }

    // `revisionId` is UUIDv7-based, so descending lexical order gives a
    // stable "recently published or revised" quick list today. A later
    // catalog usage counter can refine this list by frequency without
    // changing the menu's top-ten + More contract.
    function sharedNodeQuickList() {
        const nodes = controller.sharedGradeNodes.slice()
        nodes.sort((left, right) => String(right.revisionId).localeCompare(
            String(left.revisionId)))
        return nodes.slice(0, 10)
    }

    function hasMoreSharedNodes() {
        return controller.sharedGradeNodes.length > 10
    }

    function openSharedNodePicker(x, y) {
        sharedGradePicker.presentAt(x, y)
    }

    function addTargetsToManualAlbum(targets) {
        albumDialogs.openMembership(targets)
    }

    function openKeywordPanel() {
        keywordPopup.present()
    }

    function relinkUnavailablePhoto(photoId, locationId, title, sourcePath) {
        missingPhotoDialogs.relink(photoId, locationId, title, sourcePath)
    }

    function removeUnavailablePhoto(photoId, title, sourcePath) {
        missingPhotoDialogs.confirmRemoval(photoId, title, sourcePath)
    }

    function updatePrimaryPhoto(card) {
        selectionState.updatePrimaryPhoto(card)
    }

    function selectPhoto(card, modifiers) {
        selectionState.selectPhoto(card, modifiers)
    }

    function selectMapPhoto(cluster) {
        selectionState.selectPhoto({
            "photoId": String(cluster.photoId || ""),
            "representationId": String(cluster.representationId || ""),
            "locationId": String(cluster.locationId || ""),
            "sourcePath": String(cluster.sourcePath || ""),
            "sourceAvailable": true,
            "title": String(cluster.title || ""),
            "visualHandle": "",
            "decisionHeadSequence": 0,
            "decisionFlag": "unflagged",
            "decisionRating": 0,
            "liked": false,
            "colorLabel": "none",
            "visualRole": "",
            "visualSource": "",
            "visualWidth": 0,
            "visualHeight": 0
        }, 0)
    }

    function clearPrimaryPhoto() {
        selectionState.clearPrimaryPhoto()
    }

    function applySystemCollection(kind) {
        controller.clearFilters()
        if (kind === "liked")
            controller.filterLiked = "liked"
        else if (kind === "five-star")
            controller.filterMinimumRating = 5
    }

    function isSystemCollectionActive(kind) {
        if (controller.libraryAlbumId.length > 0)
            return false
        if (kind === "all")
            return !hasActiveLibraryFilter
        if (kind === "liked")
            return controller.filterLiked === "liked"
                && controller.filterMinimumRating === 0
                && controller.filterFlag === "all"
                && controller.filterColorLabel === "all"
                && controller.filterEditState === "all"
                && !hasLibraryFacetFilter
        if (kind === "five-star")
            return controller.filterMinimumRating === 5
                && controller.filterLiked === "all"
                && controller.filterFlag === "all"
                && controller.filterColorLabel === "all"
                && controller.filterEditState === "all"
                && !hasLibraryFacetFilter
        return false
    }

    function applyTravelCollection(countryKey, localityKey) {
        if (!personalProfile.hasLivingPlaces)
            return
        controller.clearFilters()
        controller.travelFilterEnabled = true
        if (String(countryKey).length > 0)
            controller.filterCountryKey = String(countryKey)
        if (String(localityKey).length > 0)
            controller.filterLocalityKey = String(localityKey)
    }

    function applyDailyCollection() {
        if (!personalProfile.hasLivingPlaces)
            return
        controller.clearFilters()
        controller.dailyFilterEnabled = true
    }

    function isTravelCollectionActive(countryKey, localityKey) {
        if (!personalProfile.hasLivingPlaces || controller.libraryAlbumId.length > 0)
            return false
        return controller.travelFilterEnabled
            && controller.filterCountryKey === String(countryKey)
            && controller.filterLocalityKey === String(localityKey)
            && controller.filterCaptureMonth.length === 0
            && controller.filterChineseLunarMonth === 0
            && controller.filterChineseLunarDay === 0
            && controller.filterChineseLunarMonthType === "all"
            && controller.filterCameraKey.length === 0
            && controller.filterLensKey.length === 0
            && controller.filterFlag === "all"
            && controller.filterMinimumRating === 0
            && controller.filterColorLabel === "all"
            && controller.filterEditState === "all"
            && controller.filterLiked === "all"
            && controller.filterExcludedFlag === "all"
            && controller.filterExcludedColorLabel === "all"
            && controller.filterKeywordIdsAll.length === 0
            && controller.filterExcludedKeywordIdsAny.length === 0
    }

    function isDailyCollectionActive() {
        if (!personalProfile.hasLivingPlaces || controller.libraryAlbumId.length > 0)
            return false
        return controller.dailyFilterEnabled
            && controller.filterCaptureMonth.length === 0
            && controller.filterChineseLunarMonth === 0
            && controller.filterChineseLunarDay === 0
            && controller.filterChineseLunarMonthType === "all"
            && controller.filterCameraKey.length === 0
            && controller.filterLensKey.length === 0
            && controller.filterCountryKey.length === 0
            && controller.filterLocalityKey.length === 0
            && controller.filterFlag === "all"
            && controller.filterMinimumRating === 0
            && controller.filterColorLabel === "all"
            && controller.filterEditState === "all"
            && controller.filterLiked === "all"
            && controller.filterExcludedFlag === "all"
            && controller.filterExcludedColorLabel === "all"
            && controller.filterKeywordIdsAll.length === 0
            && controller.filterExcludedKeywordIdsAny.length === 0
    }

    function clearSelection() {
        selectionState.clearSelection()
    }

    function openSelectedPhoto() {
        if (!canOpenSelectedPhoto)
            return
        if (selectedIsRemote) {
            precisionOpenStatus = qsTr("Downloading the original RAW from the remote Library…")
            controller.materializeRemotePhoto(selectedPhotoId)
            return
        }
        if (!controller.confirmLocalSourceAvailable(
                selectedPhotoId, selectedLocationId, selectedPath)) {
            precisionOpenStatus = qsTr("Original file not found. Relink its folder or remove it from the Library.")
            missingPhotoDialogs.presentMissing(
                selectedPhotoId, selectedLocationId, selectedTitle, selectedPath)
            return
        }
        openPrecisionRequested(selectedPhotoId, selectedRepresentationId,
                               selectedPath, selectedTitle, selectedVisualSource)
    }

    function reportPrecisionOpenFailure(message) {
        precisionOpenStatus = String(message)
    }

    function remoteOpenFailureMessage(statusCode) {
        switch (String(statusCode)) {
        case "connection-required":
        case "token-required":
            return qsTr("Connect to the remote Library in Settings, then try again.")
        case "remote-original-unavailable":
            return qsTr("The remote server does not currently allow this RAW to be downloaded.")
        case "remote-photo-unavailable":
            return qsTr("This remote photo is no longer available in the local mirror.")
        case "materialize-failed":
            return qsTr("The remote RAW could not be downloaded. Check the server connection and try again.")
        default:
            return ""
        }
    }

    function setSelectedFlag(flag) {
        if (canMutateDecision)
            controller.setPhotoFlag(selectedPhotoId, flag)
    }

    function setSelectedRating(rating) {
        if (canMutateDecision)
            controller.setPhotoRating(selectedPhotoId, rating)
    }

    Connections {
        target: review.controller
        function onItemCountChanged() {
            if (review.controller.itemCount === 0)
                review.clearSelection()
        }
        function onDecisionChanged(photoId, headSequence, flag, rating) {
            selectionState.applyDecisionChanged(photoId, headSequence, flag, rating)
        }
        function onColorLabelChanged(photoId, colorLabel) {
            selectionState.applyColorLabelChanged(photoId, colorLabel)
        }
        function onLikedChanged(photoId, liked) {
            selectionState.applyLikedChanged(photoId, liked)
        }
        function onSourceAvailabilityChanged(photoId, available) {
            selectionState.applySourceAvailabilityChanged(photoId, available)
        }
        function onRemotePhotoReady(photoId, representationId, sourcePath, title) {
            review.precisionOpenStatus = ""
            review.openPrecisionRequested(
                photoId, representationId, sourcePath, title, "")
        }
        function onRemoteLibraryChanged() {
            if (review.controller.remoteLibraryMaterializing)
                return
            const failure = review.remoteOpenFailureMessage(
                review.controller.remoteLibraryStatusCode)
            if (failure.length > 0)
                review.precisionOpenStatus = failure
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: review.visible && review.comparison.compareMode
            && !review.controller.comparisonBusy
        onActivated: review.comparison.exitComparison()
    }

    Shortcut {
        sequence: "C"
        enabled: review.visible && review.selectedPhotoCount === 1
            && !review.comparison.compareMode && !review.culling.arenaActive
        onActivated: review.toggleSelectedCandidate()
    }

    Shortcut {
        sequence: "P"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedFlag("picked")
    }

    Shortcut {
        sequence: "U"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedFlag("unflagged")
    }

    Shortcut {
        sequence: "X"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedFlag("rejected")
    }

    Shortcut {
        sequence: "0"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(0)
    }

    Shortcut {
        sequence: "1"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(1)
    }

    Shortcut {
        sequence: "2"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(2)
    }

    Shortcut {
        sequence: "3"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(3)
    }

    Shortcut {
        sequence: "4"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(4)
    }

    Shortcut {
        sequence: "5"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(5)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ReviewLibrarySidebar {
            workspace: review
            albumDialogs: albumDialogs
        }

        ReviewGallerySurface {
            id: gallerySurface
            Layout.fillWidth: true
            Layout.fillHeight: true
            workspace: review
            onOpenLibraryManagementRequested:
                review.openLibraryManagementRequested()
            onOpenMetadataRequested: metadataWindow.present()
            onSharedGradeRequested: anchorItem =>
                sharedGradePicker.presentFrom(anchorItem)
            onExportRequested: targets => review.exportRequested(targets)
        }

        ReviewPhotoInspector {
            visible: !review.comparison.compareMode
            Layout.preferredWidth: visible ? 278 : 0
            review: review
            metadataPresentation: metadataPresentation
            onOpenMetadataRequested: metadataWindow.present()
        }
    }
}
