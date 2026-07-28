pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: review

    required property var controller
    required property var justifiedReviewLayout
    required property var preferences

    ReviewSelectionState {
        id: selectionState
        controller: review.controller
        onPrimaryContextInvalidated: review.precisionOpenStatus = ""
    }

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
    readonly property alias selectedPath: selectionState.selectedPath
    readonly property alias selectedRole: selectionState.selectedRole
    readonly property alias selectedVisualSource: selectionState.selectedVisualSource
    readonly property alias selectedWidth: selectionState.selectedWidth
    readonly property alias selectedHeight: selectionState.selectedHeight
    readonly property alias selectedHasMetadata: selectionState.selectedHasMetadata
    readonly property alias selectedCameraMake: selectionState.selectedCameraMake
    readonly property alias selectedCameraModel: selectionState.selectedCameraModel
    readonly property alias selectedLensMake: selectionState.selectedLensMake
    readonly property alias selectedLensModel: selectionState.selectedLensModel
    readonly property alias selectedCapturedAtUnixSeconds:
        selectionState.selectedCapturedAtUnixSeconds
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
    property var leftComparisonSnapshot: null
    property var rightComparisonSnapshot: null
    property bool leftComparisonVisualReady: false
    property bool rightComparisonVisualReady: false
    property bool comparisonBackendReady: false
    property string comparisonPresentationId: ""
    property string leftComparisonRequestTicket: ""
    property string rightComparisonRequestTicket: ""
    property string leftComparisonSource: ""
    property string rightComparisonSource: ""
    property bool compareMode: false
    // The grid stays the broad library browser. The single presentation is a
    // deliberately focused culling surface that still consumes the same
    // filtered catalogue model.
    enum GalleryPresentation {
        JustifiedGrid,
        SinglePhotoFilmstrip
    }
    property int galleryPresentation: ReviewWorkspace.JustifiedGrid
    property string localComparisonStatusKey: ""
    property int localComparisonStatusSlot: -1
    property string precisionOpenStatus: ""

    readonly property bool comparisonReady: leftComparisonSnapshot !== null
        && rightComparisonSnapshot !== null
    readonly property bool comparisonVisualsReady: leftComparisonVisualReady
        && rightComparisonVisualReady
    readonly property bool canSubmitComparison: comparisonReady
        && comparisonVisualsReady && comparisonBackendReady
        && compareMode && !controller.scanning && !controller.refreshing
        && !controller.comparisonBusy && !controller.decisionBusy
    readonly property bool canMutateDecision: selectedPhotoId.length > 0
        && !compareMode && !controller.scanning && !controller.refreshing
        && !controller.busy && !controller.loadingMore
        && !controller.comparisonBusy && !controller.decisionBusy
    readonly property bool canOpenSelectedPhoto: selectedPhotoId.length > 0
        && selectedRepresentationId.length > 0 && selectedPath.length > 0
        && !compareMode
        && !controller.refreshing
        && !controller.busy && !controller.loadingMore
        && !controller.comparisonBusy && !controller.decisionBusy
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
        || controller.filterCameraKey.length > 0
        || controller.filterLensKey.length > 0
    readonly property bool hasActiveLibraryFilter:
        controller.filterFlag !== "all"
        || controller.filterMinimumRating > 0
        || controller.filterColorLabel !== "all"
        || controller.filterEditState !== "all"
        || controller.filterLiked !== "all"
        || hasLibraryFacetFilter
    readonly property var manualLibraryAlbums: {
        const albums = controller.libraryAlbums
        const manualAlbums = []
        for (let index = 0; index < albums.length; ++index) {
            if (String(albums[index].kind) === "manual")
                manualAlbums.push(albums[index])
        }
        return manualAlbums
    }
    readonly property string localComparisonStatus: {
        if (localComparisonStatusKey === "photo-in-both-slots")
            return qsTr("A photo cannot occupy both comparison slots.")
        if (localComparisonStatusKey === "slot-updated")
            return localComparisonStatusSlot === 0
                ? qsTr("Left evidence slot updated.")
                : qsTr("Right evidence slot updated.")
        return ""
    }

    signal openPrecisionRequested(string photoId, string representationId,
                                  string sourcePath, string photoTitle,
                                  string previewSource)
    signal openLibraryManagementRequested()
    signal exportRequested(var targets)

    MetadataWindow {
        id: metadataWindow
        transientParent: review.Window.window
        preferences: review.preferences
        photoTitle: review.selectedTitle
        sourcePath: review.selectedPath
        hasMetadata: review.selectedHasMetadata
        metadataPending: review.controller.photoInspectionBusy
        metadataFailed: review.controller.photoInspectionFailed
        fields: review.metadataFields()
        onRetryRequested: review.controller.retryPhotoInspection()
    }

    LibraryFacetPopup {
        id: libraryFacetPopup
        controller: review.controller
    }

    LibraryAlbumDialogs {
        id: albumDialogs
        anchors.fill: parent
        controller: review.controller
        hasActiveLibraryFilter: review.hasActiveLibraryFilter
        manualAlbums: review.manualLibraryAlbums
    }

    Popup {
        id: sharedBatchPopup
        width: 292
        padding: 8
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.panelRaised
            radius: Theme.controlRadius
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: Column {
            spacing: 4

            Label {
                width: parent.width
                leftPadding: 8
                rightPadding: 8
                topPadding: 6
                bottomPadding: 8
                text: qsTr("APPLY SHARED NODE · %L1 PHOTOS").arg(
                    review.selectedPhotoCount)
                color: Theme.textMuted
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 0.7
            }

            Label {
                width: parent.width
                leftPadding: 8
                rightPadding: 8
                topPadding: 4
                bottomPadding: 8
                visible: review.controller.sharedGradeNodes.length === 0
                text: qsTr("No shared Grade Nodes yet")
                color: Theme.textMuted
                font.pixelSize: 10
            }

            ListView {
                id: sharedBatchList
                width: parent.width
                height: Math.min(contentHeight, 296)
                visible: count > 0
                clip: true
                spacing: 2
                model: review.controller.sharedGradeNodes

                delegate: Rectangle {
                    id: sharedBatchRow
                    required property var modelData
                    width: sharedBatchList.width
                    height: 38
                    radius: Theme.compactControlRadius
                    color: sharedBatchMouse.containsMouse
                        ? Theme.buttonGhostHover : Theme.transparent

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        ShadowIcon {
                            source: "qrc:/icons/shared-link.svg"
                            color: Theme.accent
                            size: 15
                        }

                        Label {
                            Layout.fillWidth: true
                            text: String(sharedBatchRow.modelData.label)
                            color: Theme.textPrimary
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }

                        Label {
                            text: qsTr("V%1").arg(
                                Number(sharedBatchRow.modelData.revisionNumber))
                            color: Theme.textMuted
                            font.pixelSize: 9
                        }
                    }

                    MouseArea {
                        id: sharedBatchMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            review.controller.applySharedGradeNode(
                                String(sharedBatchRow.modelData.layerId),
                                review.batchSelectionTargets())
                            sharedBatchPopup.close()
                        }
                    }
                }
            }
        }
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

    function formatLuma(value) {
        return Number(value).toLocaleString(Qt.locale(), "f", 3)
    }

    function formatPercent(value) {
        return qsTr("%1%").arg(
            (Number(value) * 100.0).toLocaleString(Qt.locale(), "f", 2))
    }

    function formatProxyDetail(value) {
        const number = Number(value)
        const magnitude = Math.abs(number)
        if (magnitude > 0.0 && (magnitude < 0.001 || magnitude >= 1000.0))
            return number.toLocaleString(Qt.locale(), "e", 3)
        return number.toLocaleString(Qt.locale(), "f", 4)
    }

    function concisePreprocessingVersion(value) {
        const parts = String(value).split(":")
        if (parts.length < 2)
            return value.length > 0 ? value : "—"
        return parts[0] + " · " + parts[parts.length - 1]
    }

    function joinedIdentity(make, model) {
        const parts = []
        if (String(make).trim().length > 0)
            parts.push(String(make).trim())
        if (String(model).trim().length > 0
                && String(model).trim() !== String(make).trim())
            parts.push(String(model).trim())
        return parts.length > 0 ? parts.join(" ") : "—"
    }

    function formatShutter(seconds) {
        const value = Number(seconds)
        if (!(value > 0))
            return "—"
        if (value >= 1)
            return qsTr("%1 s").arg(value.toLocaleString(Qt.locale(), "f", value < 10 ? 1 : 0))
        const reciprocal = Math.round(1 / value)
        return reciprocal > 1 ? qsTr("1/%1 s").arg(reciprocal)
                              : qsTr("%1 s").arg(value.toLocaleString(Qt.locale(), "f", 2))
    }

    function exifValue(field) {
        switch (field) {
        case "captured_at":
            return Number(selectedCapturedAtUnixSeconds) > 0
                ? new Date(Number(selectedCapturedAtUnixSeconds) * 1000).toLocaleString(Qt.locale()) : "—"
        case "camera": return joinedIdentity(selectedCameraMake, selectedCameraModel)
        case "lens": return joinedIdentity(selectedLensMake, selectedLensModel)
        case "exposure": return formatShutter(selectedExposureTimeSeconds)
        case "aperture": return selectedApertureFNumber > 0
            ? qsTr("f/%1").arg(selectedApertureFNumber.toLocaleString(Qt.locale(), "f", 1)) : "—"
        case "iso": return selectedIsoSpeed > 0 ? qsTr("ISO %1").arg(Math.round(selectedIsoSpeed)) : "—"
        case "focal_length": return selectedFocalLengthMm > 0
            ? qsTr("%1 mm").arg(selectedFocalLengthMm.toLocaleString(Qt.locale(), "f", 1)) : "—"
        case "dimensions": return selectedWidth > 0 ? qsTr("%L1 × %L2").arg(selectedWidth).arg(selectedHeight) : "—"
        case "focal_length_35mm": return selectedFocalLength35mm > 0
            ? qsTr("%1 mm equiv.").arg(selectedFocalLength35mm.toLocaleString(Qt.locale(), "f", 0)) : "—"
        case "raw_dimensions": return selectedRawWidth > 0 ? qsTr("%L1 × %L2").arg(selectedRawWidth).arg(selectedRawHeight) : "—"
        case "sensor_bits": return selectedSensorBits > 0 ? qsTr("%1-bit").arg(selectedSensorBits) : "—"
        case "cfa": return selectedCfaPattern.length > 0 ? selectedCfaPattern : "—"
        case "dng": return selectedDngVersion.length > 0 ? selectedDngVersion : "—"
        default: return "—"
        }
    }

    function metadataFields() {
        return [
            { id: "captured_at", group: qsTr("Capture"), firstInGroup: true,
              label: qsTr("Capture time"), value: exifValue("captured_at") },
            { id: "exposure", group: qsTr("Capture"), firstInGroup: false,
              label: qsTr("Shutter speed"), value: exifValue("exposure") },
            { id: "aperture", group: qsTr("Capture"), firstInGroup: false,
              label: qsTr("Aperture"), value: exifValue("aperture") },
            { id: "iso", group: qsTr("Capture"), firstInGroup: false,
              label: qsTr("ISO sensitivity"), value: exifValue("iso") },
            { id: "camera", group: qsTr("Camera and lens"), firstInGroup: true,
              label: qsTr("Camera"), value: exifValue("camera") },
            { id: "lens", group: qsTr("Camera and lens"), firstInGroup: false,
              label: qsTr("Lens"), value: exifValue("lens") },
            { id: "focal_length", group: qsTr("Camera and lens"), firstInGroup: false,
              label: qsTr("Focal length"), value: exifValue("focal_length") },
            { id: "focal_length_35mm", group: qsTr("Camera and lens"), firstInGroup: false,
              label: qsTr("35 mm equivalent"), value: exifValue("focal_length_35mm") },
            { id: "dimensions", group: qsTr("Image"), firstInGroup: true,
              label: qsTr("Preview dimensions"), value: exifValue("dimensions") },
            { id: "raw_dimensions", group: qsTr("Image"), firstInGroup: false,
              label: qsTr("RAW dimensions"), value: exifValue("raw_dimensions") },
            { id: "sensor_bits", group: qsTr("Image"), firstInGroup: false,
              label: qsTr("Sensor bit depth"), value: exifValue("sensor_bits") },
            { id: "cfa", group: qsTr("Image"), firstInGroup: false,
              label: qsTr("Color filter array"), value: exifValue("cfa") },
            { id: "dng", group: qsTr("Image"), firstInGroup: false,
              label: qsTr("DNG version"), value: exifValue("dng") }
        ]
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
        controller.refreshSharedGradeNodes()
        sharedBatchPopup.x = Math.max(8, Math.min(Number(x),
            review.width - sharedBatchPopup.width - 8))
        sharedBatchPopup.y = Math.max(8, Math.min(Number(y),
            review.height - sharedBatchPopup.height - 8))
        sharedBatchPopup.open()
    }

    function addTargetsToManualAlbum(targets) {
        albumDialogs.openMembership(targets)
    }

    function updatePrimaryPhoto(card) {
        selectionState.updatePrimaryPhoto(card)
    }

    function selectPhoto(card, modifiers) {
        selectionState.selectPhoto(card, modifiers)
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

    function clearSelection() {
        selectionState.clearSelection()
    }

    function selectedComparisonSnapshot() {
        return {
            "photoId": String(selectedPhotoId),
            "representationId": String(selectedRepresentationId),
            "visualHandle": String(selectedVisualHandle),
            "title": String(selectedTitle),
            "sourcePath": String(selectedPath),
            "visualRole": String(selectedRole),
            "visualSource": String(selectedVisualSource),
            "visualWidth": Number(selectedWidth),
            "visualHeight": Number(selectedHeight),
            "hasTechnicalObservation": Boolean(selectedHasTechnicalObservation),
            "technicalInputWidth": Number(selectedTechnicalInputWidth),
            "technicalInputHeight": Number(selectedTechnicalInputHeight),
            "technicalPreprocessingVersion": String(
                selectedTechnicalPreprocessingVersion),
            "technicalImplementationVersion": String(
                selectedTechnicalImplementationVersion),
            "meanLuma": Number(selectedMeanLuma),
            "p01Luma": Number(selectedP01Luma),
            "p50Luma": Number(selectedP50Luma),
            "p99Luma": Number(selectedP99Luma),
            "nearBlackFraction": Number(selectedNearBlackFraction),
            "nearWhiteFraction": Number(selectedNearWhiteFraction),
            "laplacianVariance": Number(selectedLaplacianVariance),
            "edgeEnergy": Number(selectedEdgeEnergy)
        }
    }

    function sameComparisonIdentity(left, right) {
        return left !== null && right !== null
            && left.photoId === right.photoId
            && left.representationId === right.representationId
            && left.visualHandle === right.visualHandle
            && left.visualSource === right.visualSource
    }

    function setLocalComparisonStatus(statusKey, slot) {
        localComparisonStatusSlot = slot
        localComparisonStatusKey = statusKey
    }

    function clearLocalComparisonStatus() {
        localComparisonStatusKey = ""
        localComparisonStatusSlot = -1
    }

    function setSelectedAsLeft() {
        if (selectedPhotoId.length === 0 || selectedRepresentationId.length === 0
                || selectedVisualHandle.length === 0
                || selectedVisualSource.length === 0)
            return
        if (rightComparisonSnapshot !== null
                && rightComparisonSnapshot.photoId === selectedPhotoId) {
            setLocalComparisonStatus("photo-in-both-slots", -1)
            return
        }
        const snapshot = selectedComparisonSnapshot()
        leftComparisonVisualReady = false
        comparisonBackendReady = false
        leftComparisonSnapshot = snapshot
        setLocalComparisonStatus("slot-updated", 0)
    }

    function setSelectedAsRight() {
        if (selectedPhotoId.length === 0 || selectedRepresentationId.length === 0
                || selectedVisualHandle.length === 0
                || selectedVisualSource.length === 0)
            return
        if (leftComparisonSnapshot !== null
                && leftComparisonSnapshot.photoId === selectedPhotoId) {
            setLocalComparisonStatus("photo-in-both-slots", -1)
            return
        }
        const snapshot = selectedComparisonSnapshot()
        rightComparisonVisualReady = false
        comparisonBackendReady = false
        rightComparisonSnapshot = snapshot
        setLocalComparisonStatus("slot-updated", 1)
    }

    function resetPreparedComparison(cancelBackend) {
        if (cancelBackend && comparisonPresentationId.length > 0)
            controller.cancelComparison(comparisonPresentationId)
        comparisonPresentationId = ""
        leftComparisonRequestTicket = ""
        rightComparisonRequestTicket = ""
        leftComparisonSource = ""
        rightComparisonSource = ""
        leftComparisonVisualReady = false
        rightComparisonVisualReady = false
        comparisonBackendReady = false
    }

    function clearComparisonSlots(cancelBackend) {
        resetPreparedComparison(cancelBackend !== false)
        leftComparisonSnapshot = null
        rightComparisonSnapshot = null
        compareMode = false
        clearLocalComparisonStatus()
    }

    function enterComparison() {
        if (!comparisonReady)
            return
        const prepared = controller.prepareComparison(
            leftComparisonSnapshot.visualHandle,
            rightComparisonSnapshot.visualHandle)
        if (!prepared || String(prepared.presentationId).length === 0)
            return
        comparisonPresentationId = String(prepared.presentationId)
        leftComparisonRequestTicket = String(prepared.leftRequestTicket)
        rightComparisonRequestTicket = String(prepared.rightRequestTicket)
        leftComparisonSource = String(prepared.leftSource)
        rightComparisonSource = String(prepared.rightSource)
        leftComparisonVisualReady = false
        rightComparisonVisualReady = false
        comparisonBackendReady = false
        compareMode = true
        clearLocalComparisonStatus()
    }

    function exitComparison() {
        resetPreparedComparison(true)
        compareMode = false
        clearLocalComparisonStatus()
    }

    function refreshComparisonReadiness() {
        comparisonBackendReady = false
        if (!compareMode || !comparisonVisualsReady
                || comparisonPresentationId.length === 0)
            return
        comparisonBackendReady = controller.confirmComparisonReady(
            comparisonPresentationId,
            leftComparisonRequestTicket,
            rightComparisonRequestTicket)
    }

    function submitComparison(outcome) {
        if (!canSubmitComparison)
            return
        controller.recordComparison(comparisonPresentationId, outcome)
    }

    function openSelectedPhoto() {
        if (!canOpenSelectedPhoto)
            return
        openPrecisionRequested(selectedPhotoId, selectedRepresentationId,
                               selectedPath, selectedTitle, selectedVisualSource)
    }

    function reportPrecisionOpenFailure(message) {
        precisionOpenStatus = String(message)
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
            if (review.controller.itemCount === 0) {
                review.clearSelection()
                review.clearComparisonSlots()
            }
        }
        function onComparisonRecorded() {
            review.clearComparisonSlots(false)
            review.clearLocalComparisonStatus()
            justifiedGrid.forceActiveFocus()
        }
        function onComparisonForgotten() {
            review.clearLocalComparisonStatus()
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
    }

    Shortcut {
        sequence: "1"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(0)
    }

    Shortcut {
        sequence: "2"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(1)
    }

    Shortcut {
        sequence: "3"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(2)
    }

    Shortcut {
        sequence: "4"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(3)
    }

    Shortcut {
        sequence: "0"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(4)
    }

    Shortcut {
        sequence: "Escape"
        enabled: review.visible && review.compareMode
            && !review.controller.comparisonBusy
        onActivated: review.exitComparison()
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

        Rectangle {
            Layout.preferredWidth: 210
            Layout.fillHeight: true
            color: review.panel

            Rectangle {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 1
                color: review.border
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.panelPadding
                anchors.rightMargin: Theme.panelPadding + 1
                anchors.topMargin: Theme.panelPadding
                anchors.bottomMargin: Theme.panelPadding
                spacing: 8

                Label {
                    text: qsTr("LIBRARY")
                    color: review.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.6
                }

                Item { Layout.preferredHeight: 6 }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    radius: 7
                    color: review.isSystemCollectionActive("all")
                        ? Theme.accentSurface : Theme.transparent

                    Rectangle {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 3
                        width: 3
                        height: 18
                        radius: 1.5
                        color: review.isSystemCollectionActive("all")
                            ? review.accent : Theme.transparent
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 10
                        Label { text: qsTr("All Photos"); color: review.textPrimary }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("%L1").arg(review.controller.itemCount)
                            color: review.textMuted
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: review.applySystemCollection("all")
                    }
                }

                Repeater {
                    model: [
                        {
                            id: "recent-imports",
                            title: qsTr("Recent Imports"),
                            icon: "qrc:/icons/history.svg",
                            enabled: false,
                            hint: qsTr("Recent import sessions will appear here when import-time filtering is available.")
                        },
                        {
                            id: "liked",
                            title: qsTr("Liked"),
                            icon: "qrc:/icons/heart.svg",
                            enabled: true,
                            hint: qsTr("Show photos marked Like")
                        },
                        {
                            id: "five-star",
                            title: qsTr("5 Stars"),
                            icon: "qrc:/icons/star.svg",
                            enabled: true,
                            hint: qsTr("Show photos rated 5 stars")
                        }
                    ]

                    delegate: Rectangle {
                        id: defaultCollectionRow
                        required property var modelData
                        readonly property bool selected:
                            review.isSystemCollectionActive(String(modelData.id))
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        radius: Theme.compactControlRadius
                        color: selected ? Theme.accentSurface
                            : defaultCollectionMouse.containsMouse && modelData.enabled
                                ? Theme.buttonGhostHover : Theme.transparent

                        Rectangle {
                            anchors.left: parent.left
                            anchors.leftMargin: 3
                            anchors.verticalCenter: parent.verticalCenter
                            width: 2
                            height: 16
                            radius: 1
                            color: defaultCollectionRow.selected
                                ? review.accent : Theme.transparent
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 13
                            anchors.rightMargin: 9
                            spacing: 7

                            ShadowIcon {
                                source: String(defaultCollectionRow.modelData.icon)
                                color: defaultCollectionRow.modelData.enabled
                                    ? (defaultCollectionRow.selected
                                        ? review.accent : review.textMuted)
                                    : Theme.textDisabled
                                size: 14
                            }

                            Label {
                                Layout.fillWidth: true
                                text: String(defaultCollectionRow.modelData.title)
                                color: defaultCollectionRow.modelData.enabled
                                    ? (defaultCollectionRow.selected
                                        ? review.textPrimary : review.textSecondary)
                                    : Theme.textDisabled
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }

                            Label {
                                visible: !defaultCollectionRow.modelData.enabled
                                text: qsTr("SOON")
                                color: Theme.textDisabled
                                font.pixelSize: 8
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.5
                            }
                        }

                        MouseArea {
                            id: defaultCollectionMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: defaultCollectionRow.modelData.enabled
                                ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: {
                                if (defaultCollectionRow.modelData.enabled) {
                                    review.applySystemCollection(
                                        String(defaultCollectionRow.modelData.id))
                                }
                            }
                        }

                        ToolTip {
                            parent: defaultCollectionRow
                            visible: defaultCollectionMouse.containsMouse
                                && !defaultCollectionRow.modelData.enabled
                            delay: 400
                            text: String(defaultCollectionRow.modelData.hint)
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    Layout.bottomMargin: 2
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("ALBUMS")
                        color: review.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.1
                    }

                    BusyIndicator {
                        Layout.preferredWidth: 14
                        Layout.preferredHeight: 14
                        visible: review.controller.libraryAlbumsBusy
                        running: visible
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/node-add.svg"
                        buttonSize: 24
                        iconSize: 15
                        toolTipText: qsTr("Create album")
                        accessibleName: toolTipText
                        enabled: !review.controller.libraryAlbumsBusy
                        onClicked: albumDialogs.openCreate()
                    }
                }

                ListView {
                    id: albumList
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(contentHeight, 188)
                    visible: count > 0
                    clip: true
                    spacing: 2
                    model: review.controller.libraryAlbums

                    delegate: Rectangle {
                        id: albumRow
                        required property var modelData
                        readonly property string albumId: String(modelData.id)
                        readonly property bool selected:
                            review.controller.libraryAlbumId === albumId
                        width: albumList.width
                        height: 32
                        radius: Theme.compactControlRadius
                        color: selected ? Theme.accentSurface
                            : albumMouse.containsMouse
                                ? Theme.buttonGhostHover : Theme.transparent

                        Rectangle {
                            anchors.left: parent.left
                            anchors.leftMargin: 3
                            anchors.verticalCenter: parent.verticalCenter
                            width: 2
                            height: 16
                            radius: 1
                            color: albumRow.selected ? review.accent : Theme.transparent
                        }

                        RowLayout {
                            z: 1
                            anchors.fill: parent
                            anchors.leftMargin: 11
                            anchors.rightMargin: 8
                            spacing: 7

                            ShadowIcon {
                                source: String(albumRow.modelData.kind) === "smart"
                                    ? "qrc:/icons/filter.svg"
                                    : "qrc:/icons/library-manage.svg"
                                color: albumRow.selected ? review.accent
                                    : review.textMuted
                                size: 14
                            }

                            Label {
                                Layout.fillWidth: true
                                text: String(albumRow.modelData.name)
                                color: albumRow.selected
                                    ? review.textPrimary : review.textSecondary
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }

                            Label {
                                visible: String(albumRow.modelData.kind) === "smart"
                                text: qsTr("CONDITION")
                                color: albumRow.selected ? review.accent : review.textMuted
                                font.pixelSize: 8
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.55
                            }

                            ShadowIconButton {
                                visible: albumRow.selected || albumMouse.containsMouse
                                source: "qrc:/icons/settings.svg"
                                buttonSize: 22
                                iconSize: 13
                                toolTipText: qsTr("Manage album")
                                accessibleName: toolTipText
                                enabled: !review.controller.libraryAlbumsBusy
                                onClicked: albumDialogs.openManage(
                                    albumRow.albumId,
                                    String(albumRow.modelData.name),
                                    String(albumRow.modelData.kind))
                            }
                        }

                        MouseArea {
                            id: albumMouse
                            anchors.fill: parent
                            z: 0
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: review.controller.libraryAlbumId = albumRow.albumId
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: importStatusColumn.implicitHeight + 20
                    visible: review.controller.scanning
                        || (review.controller.refreshing
                            && Number(review.controller.scanProgress.scanId) > 0)
                    radius: 7
                    color: Theme.panelInset
                    border.color: Theme.borderStrong

                    ColumnLayout {
                        id: importStatusColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 5

                        RowLayout {
                            Layout.fillWidth: true

                            Label {
                                text: review.controller.refreshing
                                        && !review.controller.scanning
                                    ? qsTr("LIBRARY REFRESH")
                                    : review.controller.scanProgress.phase === "cancelling"
                                    ? qsTr("STOPPING IMPORT") : qsTr("IMPORTING")
                                color: review.accent
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.9
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%L1 catalogued").arg(
                                    review.controller.scanProgress.cataloguedFiles)
                                color: review.textPrimary
                                horizontalAlignment: Text.AlignRight
                                font.pixelSize: 9
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: review.controller.scanning
                                ? qsTr("%L1 supported · %L2 preview checks queued · %L3 filesystem issues")
                                    .arg(review.controller.scanProgress.supportedFiles)
                                    .arg(review.controller.scanProgress.decodeQueued)
                                    .arg(review.controller.scanProgress.issueCount)
                                : qsTr("%L1 supported · %L2/%L3 preview checks completed · %L4 filesystem issues")
                                    .arg(review.controller.scanProgress.supportedFiles)
                                    .arg(review.controller.scanProgress.decodeCompleted)
                                    .arg(review.controller.scanProgress.decodeQueued)
                                    .arg(review.controller.scanProgress.issueCount)
                            color: review.textMuted
                            elide: Text.ElideRight
                            font.pixelSize: 9
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !review.controller.scanning
                            text: qsTr("%L1 decode failures · %L2 preview failures · %L3 cancelled")
                                .arg(review.controller.scanProgress.decodeHardFailures)
                                .arg(review.controller.scanProgress.previewFailures)
                                .arg(review.controller.scanProgress.decodeCancelled)
                            color: review.textMuted
                            elide: Text.ElideRight
                            font.pixelSize: 9
                        }

                        ProgressBar {
                            Layout.fillWidth: true
                            indeterminate: true
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    Layout.topMargin: 10
                    visible: review.controller.sessionEvidenceCount > 0
                        || review.controller.canUndoComparison
                    color: review.border
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: review.controller.sessionEvidenceCount > 0
                        || review.controller.canUndoComparison
                    spacing: 6

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: qsTr("COMPARE EVIDENCE")
                            color: review.textMuted
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("%L1 active this session").arg(
                                review.controller.sessionEvidenceCount)
                            color: review.textPrimary
                            font.pixelSize: 11
                        }
                    }

                    ShadowIconButton {
                        id: undoComparisonButton
                        source: "qrc:/icons/undo.svg"
                        toolTipText: qsTr("Forget last comparison")
                        accessibleName: toolTipText
                        visible: enabled
                        enabled: review.controller.canUndoComparison
                            && !review.controller.comparisonBusy
                            && !review.controller.decisionBusy
                            && !review.controller.scanning
                            && !review.controller.refreshing
                            && !review.controller.busy
                            && !review.controller.loadingMore
                        onClicked: review.controller.undoLastComparison()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: !review.compareMode
                        && (review.controller.comparisonStatusText.length > 0
                            || review.localComparisonStatus.length > 0)
                    text: review.localComparisonStatus.length > 0
                        ? review.localComparisonStatus
                        : review.controller.comparisonStatusText
                    color: review.controller.comparisonBusy
                        ? review.accent : review.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: 9
                }

                Item { Layout.fillHeight: true }

                Label {
                    text: qsTr("LOCAL · MACOS")
                    color: Theme.textQuiet
                    font.pixelSize: 9
                    font.letterSpacing: 1.2
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.window

            Rectangle {
                id: reviewToolBar
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 42
                z: 3
                visible: !review.compareMode
                color: Theme.chrome

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: review.border
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 12
                    spacing: 8

                    Label {
                        text: review.currentLibraryAlbumName.length > 0
                            ? review.currentLibraryAlbumName.toUpperCase()
                            : qsTr("ALL PHOTOS")
                        color: review.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                    }

                    Label {
                        text: qsTr("%L1 visible").arg(
                            review.controller.filteredItemCount)
                        color: review.textPrimary
                        font.pixelSize: 11
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/library-manage.svg"
                        toolTipText: qsTr("Manage photo sources")
                        accessibleName: toolTipText
                        onClicked: review.openLibraryManagementRequested()
                    }

                    ShadowIconButton {
                        checkable: true
                        checked: review.hasLibraryFacetFilter
                        source: "qrc:/icons/filter.svg"
                        toolTipText: qsTr("Browse Library facets")
                        accessibleName: toolTipText
                        onClicked: libraryFacetPopup.open()
                    }

                    Item { Layout.fillWidth: true }

                    ShadowIconButton {
                        source: "qrc:/icons/review-grid.svg"
                        selected: review.galleryPresentation
                            === ReviewWorkspace.JustifiedGrid
                        toolTipText: qsTr("Browse as a photo grid")
                        accessibleName: toolTipText
                        onClicked: review.galleryPresentation
                            = ReviewWorkspace.JustifiedGrid
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/filmstrip.svg"
                        selected: review.galleryPresentation
                            === ReviewWorkspace.SinglePhotoFilmstrip
                        toolTipText: qsTr("Review one photo with a filmstrip")
                        accessibleName: toolTipText
                        onClicked: review.galleryPresentation
                            = ReviewWorkspace.SinglePhotoFilmstrip
                    }

                    Label {
                        visible: review.galleryPresentation
                            === ReviewWorkspace.JustifiedGrid
                        text: qsTr("SCALE")
                        color: review.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.7
                    }

                    ShadowInlineSlider {
                        id: galleryScaleSlider
                        visible: review.galleryPresentation
                            === ReviewWorkspace.JustifiedGrid
                        Layout.preferredWidth: 138
                        from: 96
                        to: 360
                        stepSize: 4
                        value: review.justifiedReviewLayout.targetRowHeight
                        toolTipText: qsTr("Thumbnail scale")
                        Accessible.name: toolTipText
                        onMoved: {
                            const next = Math.round(value)
                            review.justifiedReviewLayout.targetRowHeight = next
                            review.preferences.libraryThumbnailScale = next
                        }
                    }

                    ShadowIconButton {
                        visible: review.galleryPresentation
                            === ReviewWorkspace.JustifiedGrid
                        source: "qrc:/icons/fit-view.svg"
                        toolTipText: qsTr("Restore default thumbnail scale")
                        accessibleName: toolTipText
                        onClicked: {
                            review.justifiedReviewLayout.targetRowHeight = 188
                            review.preferences.libraryThumbnailScale = 188
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 18
                        color: review.border
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/metadata.svg"
                        toolTipText: qsTr("Open photo metadata")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoId.length > 0
                        onClicked: metadataWindow.present()
                    }

                    Label {
                        visible: review.selectedPhotoCount > 1
                        text: qsTr("%L1 selected").arg(
                            review.selectedPhotoCount)
                        color: review.textMuted
                        font.pixelSize: 9
                    }

                    ShadowIconButton {
                        id: applySharedGradeButton
                        source: "qrc:/icons/shared-link.svg"
                        toolTipText: qsTr("Apply a shared Grade Node to selection")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoCount > 0
                        onClicked: {
                            review.controller.refreshSharedGradeNodes()
                            const position = mapToItem(
                                review, width - sharedBatchPopup.width,
                                height + 6)
                            sharedBatchPopup.x = Math.max(
                                8, Math.min(position.x,
                                    review.width - sharedBatchPopup.width - 8))
                            sharedBatchPopup.y = Math.max(
                                8, Math.min(position.y,
                                    review.height - sharedBatchPopup.height - 8))
                            sharedBatchPopup.open()
                        }
                    }

                    ShadowIconButton {
                        id: addToManualAlbumButton
                        source: "qrc:/icons/add-folder.svg"
                        toolTipText: qsTr("Add selected photos to a Manual Album")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoCount > 0
                            && review.manualLibraryAlbums.length > 0
                            && !review.controller.libraryAlbumsBusy
                        onClicked: review.addTargetsToManualAlbum(
                            review.batchSelectionTargets())
                    }

                    ShadowIconButton {
                        visible: review.currentLibraryAlbumIsManual
                        source: "qrc:/icons/clear.svg"
                        toolTipText: qsTr("Remove selected photos from this Manual Album")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoCount > 0
                            && !review.controller.libraryAlbumsBusy
                        onClicked: review.controller.removePhotosFromManualLibraryAlbum(
                            String(review.currentLibraryAlbum.id),
                            review.batchSelectionTargets())
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/export.svg"
                        toolTipText: qsTr("Export selected photos")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoCount > 0
                        onClicked: review.exportRequested(
                            review.batchSelectionTargets())
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/edit.svg"
                        variant: ShadowIconButton.Tinted
                        toolTipText: qsTr("Open selected photo in Precision")
                        accessibleName: toolTipText
                        enabled: review.canOpenSelectedPhoto
                        onClicked: review.openSelectedPhoto()
                    }
                }
            }

            ListView {
                id: justifiedGrid
                objectName: "reviewJustifiedGrid"
                anchors.top: reviewToolBar.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                anchors.topMargin: 14
                anchors.bottomMargin: 18
                clip: true
                visible: !review.compareMode
                    && review.galleryPresentation === ReviewWorkspace.JustifiedGrid
                enabled: visible
                focus: visible
                spacing: review.justifiedReviewLayout.spacing
                cacheBuffer: 900
                model: review.justifiedReviewLayout

                function maybeLoadMore() {
                    if (review.controller.hasMore
                            && !review.controller.scanning
                            && !review.controller.refreshing
                            && !review.controller.busy
                            && !review.controller.loadingMore
                            && contentY + height >= contentHeight
                                - Math.max(400,
                                    review.justifiedReviewLayout.targetRowHeight * 2)) {
                        review.controller.loadMore()
                    }
                }

                onWidthChanged: review.justifiedReviewLayout.availableWidth
                    = Math.max(0, Math.floor(width))
                Component.onCompleted: review.justifiedReviewLayout.availableWidth
                    = Math.max(0, Math.floor(width))
                onContentYChanged: maybeLoadMore()
                onHeightChanged: Qt.callLater(maybeLoadMore)
                onCountChanged: Qt.callLater(maybeLoadMore)

                delegate: Item {
                    id: justifiedRow
                    required property var items
                    required property int rowHeight

                    width: justifiedGrid.width
                    // `rowHeight` is the image height. Captions are outside the
                    // image geometry so every visible image retains its ratio.
                    height: rowHeight + 48

                    Repeater {
                        model: justifiedRow.items

                        delegate: ReviewPhotoCard {
                            required property var modelData
                            x: Number(modelData.layoutX)
                            y: 0
                            width: Number(modelData.layoutWidth)
                            height: justifiedRow.height
                            workspace: review
                            entry: modelData
                        }
                    }
                }
            }

            ReviewSinglePreview {
                id: singlePhotoPreview
                anchors.top: reviewToolBar.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: !review.compareMode
                    && review.galleryPresentation
                        === ReviewWorkspace.SinglePhotoFilmstrip
                review: review
                model: review.controller.model
            }

            Label {
                anchors.centerIn: justifiedGrid
                width: Math.min(420, justifiedGrid.width - 60)
                visible: justifiedGrid.visible && justifiedGrid.count === 0
                    && !review.controller.busy
                text: review.controller.scanning
                    ? qsTr("Searching the folder for supported photos…\nNew RAW files will appear here as they are catalogued.")
                    : review.controller.scanProgress.phase === "failed"
                    ? qsTr("Import stopped, and no RAW files are currently visible.\nAlready catalogued files remain safely stored.")
                    : qsTr("Add a folder to the local Library.\nShadow will show embedded previews immediately, then replace them with locally generated proxies.")
                color: review.textMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                lineHeight: 1.4
                z: 2
            }

            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 18
                visible: justifiedGrid.visible && review.controller.loadingMore
                running: visible
                width: 34
                height: 34
                z: 2
            }

            ReviewDecisionToolbar {
                id: galleryDecisionToolbar
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: 18
                anchors.bottomMargin: 18
                z: 4
                review: review
                floating: true
                includeColorLabels: true
                visible: !review.compareMode
                    && review.galleryPresentation === ReviewWorkspace.JustifiedGrid
                    && review.selectedPhotoId.length > 0
            }

            ReviewComparisonView {
                review: review
            }

            Rectangle {
                anchors.fill: parent
                visible: review.controller.busy
                color: Theme.busyOverlay

                Column {
                    anchors.centerIn: parent
                    spacing: 14
                    BusyIndicator {
                        anchors.horizontalCenter: parent.horizontalCenter
                        running: true
                    }
                    Label {
                        text: review.controller.scanning
                            ? qsTr("Finding the first photos")
                            : qsTr("Loading local Library")
                        color: review.textPrimary
                        font.pixelSize: 14
                    }
                }
            }
        }

        ReviewPhotoInspector {
            review: review
            onOpenMetadataRequested: metadataWindow.present()
        }
    }
}
