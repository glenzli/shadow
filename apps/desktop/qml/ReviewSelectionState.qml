pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick

QtObject {
    id: selection

    required property var controller

    property var selectedPhotoTargets: ({})
    // The anchor is an identity rather than a delegate index. Review delegates
    // are virtualized, while a Shift range can span photos that are off screen.
    property string selectionAnchorPhotoId: ""
    property string selectionAnchorRepresentationId: ""

    // The primary selection is a value snapshot. Never retain the virtualized
    // ReviewPhotoCard that supplied these values.
    property string selectedPhotoId: ""
    property string selectedRepresentationId: ""
    property string selectedVisualHandle: ""
    property var selectedDecisionHeadSequence: 0
    property string selectedDecisionFlag: "unflagged"
    property int selectedDecisionRating: 0
    property bool selectedLiked: false
    property string selectedColorLabel: "none"
    property string selectedTitle: ""
    property string selectedPath: ""
    property bool selectedSourceAvailable: true
    property string selectedRole: ""
    property string selectedVisualSource: ""
    property int selectedWidth: 0
    property int selectedHeight: 0
    // Detailed inspection is selected by exact identity and survives delegate
    // recycling. A late response for another representation is invisible even
    // if a backend regression were to publish it.
    readonly property var selectedInspection: {
        const value = controller.photoInspection
        if (!value || !Boolean(value.available)
                || String(value.photoId || "") !== selectedPhotoId
                || String(value.representationId || "")
                    !== selectedRepresentationId)
            return ({})
        return value
    }
    readonly property bool selectedHasMetadata:
        Boolean(selectedInspection.hasMetadata)
    readonly property string selectedCameraMake:
        String(selectedInspection.cameraMake || "")
    readonly property string selectedCameraModel:
        String(selectedInspection.cameraModel || "")
    readonly property string selectedLensMake:
        String(selectedInspection.lensMake || "")
    readonly property string selectedLensModel:
        String(selectedInspection.lensModel || "")
    readonly property var selectedCapturedAtUnixSeconds:
        selectedInspection.capturedAtUnixSeconds || 0
    readonly property bool selectedHasCoordinates:
        Boolean(selectedInspection.hasCoordinates)
    readonly property real selectedLatitude:
        Number(selectedInspection.latitude || 0)
    readonly property real selectedLongitude:
        Number(selectedInspection.longitude || 0)
    readonly property string selectedPlaceName:
        String(selectedInspection.placeName || "")
    readonly property string selectedResolvedPlaceName:
        String(selectedInspection.resolvedPlaceName || "")
    readonly property real selectedIsoSpeed:
        Number(selectedInspection.isoSpeed || 0)
    readonly property real selectedExposureTimeSeconds:
        Number(selectedInspection.exposureTimeSeconds || 0)
    readonly property real selectedApertureFNumber:
        Number(selectedInspection.apertureFNumber || 0)
    readonly property real selectedFocalLengthMm:
        Number(selectedInspection.focalLengthMm || 0)
    readonly property real selectedFocalLength35mm:
        Number(selectedInspection.focalLength35mm || 0)
    readonly property int selectedRawWidth:
        Number(selectedInspection.rawWidth || 0)
    readonly property int selectedRawHeight:
        Number(selectedInspection.rawHeight || 0)
    readonly property int selectedSensorBits:
        Number(selectedInspection.sensorBits || 0)
    readonly property string selectedCfaPattern:
        String(selectedInspection.cfaPattern || "")
    readonly property string selectedDngVersion:
        String(selectedInspection.dngVersion || "")
    readonly property bool selectedHasFocusObservation:
        Boolean(selectedInspection.hasFocusObservation)
    readonly property int selectedFocusObservationSchemaVersion:
        Number(selectedInspection.focusObservationSchemaVersion || 0)
    readonly property string selectedFocusObservationSource:
        String(selectedInspection.focusObservationSource || "")
    readonly property real selectedFocusObservationCenterX:
        Number(selectedInspection.focusObservationCenterX || 0)
    readonly property real selectedFocusObservationCenterY:
        Number(selectedInspection.focusObservationCenterY || 0)
    readonly property real selectedFocusObservationWidth:
        Number(selectedInspection.focusObservationWidth || 0)
    readonly property real selectedFocusObservationHeight:
        Number(selectedInspection.focusObservationHeight || 0)
    readonly property bool selectedFocusObservationConfirmed:
        Boolean(selectedInspection.focusObservationConfirmed)
    readonly property real selectedFocusObservationConfidence:
        Number(selectedInspection.focusObservationConfidence || 0)
    readonly property bool selectedHasTechnicalObservation:
        Boolean(selectedInspection.hasTechnicalObservation)
    readonly property int selectedTechnicalInputWidth:
        Number(selectedInspection.technicalInputWidth || 0)
    readonly property int selectedTechnicalInputHeight:
        Number(selectedInspection.technicalInputHeight || 0)
    readonly property string selectedTechnicalPreprocessingVersion:
        String(selectedInspection.technicalPreprocessingVersion || "")
    readonly property string selectedTechnicalImplementationVersion:
        String(selectedInspection.technicalImplementationVersion || "")
    readonly property real selectedMeanLuma:
        Number(selectedInspection.meanLuma || 0)
    readonly property real selectedP01Luma:
        Number(selectedInspection.p01Luma || 0)
    readonly property real selectedP50Luma:
        Number(selectedInspection.p50Luma || 0)
    readonly property real selectedP99Luma:
        Number(selectedInspection.p99Luma || 0)
    readonly property real selectedNearBlackFraction:
        Number(selectedInspection.nearBlackFraction || 0)
    readonly property real selectedNearWhiteFraction:
        Number(selectedInspection.nearWhiteFraction || 0)
    readonly property real selectedLaplacianVariance:
        Number(selectedInspection.laplacianVariance || 0)
    readonly property real selectedEdgeEnergy:
        Number(selectedInspection.edgeEnergy || 0)

    readonly property int selectedPhotoCount:
        Object.keys(selectedPhotoTargets).length

    // ReviewWorkspace owns the Precision-opening workflow status. Invalidate
    // that status whenever this owner changes or clears its primary identity.
    signal primaryContextInvalidated()

    function selectionKey(photoId, representationId) {
        return String(photoId) + "\u0000" + String(representationId)
    }

    function isPhotoSelected(photoId, representationId) {
        return selectedPhotoTargets[selectionKey(photoId, representationId)]
            !== undefined
    }

    function batchSelectionTargets() {
        const values = []
        const keys = Object.keys(selectedPhotoTargets)
        for (let index = 0; index < keys.length; ++index)
            values.push(selectedPhotoTargets[keys[index]])
        return values
    }

    function updatePrimaryPhoto(card) {
        const identityChanged = selectedPhotoId !== card.photoId
            || selectedRepresentationId !== card.representationId
        selectedPhotoId = card.photoId
        selectedRepresentationId = card.representationId
        selectedVisualHandle = card.visualHandle
        selectedDecisionHeadSequence = card.decisionHeadSequence
        selectedDecisionFlag = card.decisionFlag
        selectedDecisionRating = card.decisionRating
        selectedLiked = card.liked
        selectedColorLabel = card.colorLabel
        selectedTitle = card.title
        selectedPath = card.sourcePath
        selectedSourceAvailable = card.sourceAvailable === undefined
            ? true : Boolean(card.sourceAvailable)
        selectedRole = card.visualRole
        selectedVisualSource = card.visualSource
        selectedWidth = card.visualWidth
        selectedHeight = card.visualHeight
        if (identityChanged) {
            primaryContextInvalidated()
            if (selectedSourceAvailable) {
                controller.requestPhotoInspection(
                    selectedPhotoId, selectedRepresentationId)
            } else {
                controller.clearPhotoInspection()
            }
        }
    }

    function selectPhoto(card, modifiers) {
        const modifierMask = Number(modifiers || 0)
        const additive = (modifierMask & Qt.ControlModifier) !== 0
            || (modifierMask & Qt.MetaModifier) !== 0
        const rangeSelection = (modifierMask & Qt.ShiftModifier) !== 0
        const key = selectionKey(card.photoId, card.representationId)
        let updated = ({})
        if (rangeSelection && selectionAnchorPhotoId.length > 0
                && selectionAnchorRepresentationId.length > 0) {
            const range = controller.selectionRangeTargets(
                selectionAnchorPhotoId, selectionAnchorRepresentationId,
                card.photoId, card.representationId)
            if (range.length > 0) {
                if (additive) {
                    const previousKeys = Object.keys(selectedPhotoTargets)
                    for (let index = 0; index < previousKeys.length; ++index) {
                        const previousKey = previousKeys[index]
                        updated[previousKey] = selectedPhotoTargets[previousKey]
                    }
                }
                for (let index = 0; index < range.length; ++index) {
                    const target = range[index]
                    updated[selectionKey(target.photoId, target.representationId)] = target
                }
                selectedPhotoTargets = updated
                updatePrimaryPhoto(card)
                return
            }
        }
        if (additive) {
            const previousKeys = Object.keys(selectedPhotoTargets)
            for (let index = 0; index < previousKeys.length; ++index) {
                const previousKey = previousKeys[index]
                updated[previousKey] = selectedPhotoTargets[previousKey]
            }
            if (updated[key] !== undefined) {
                delete updated[key]
                selectedPhotoTargets = updated
                if (selectedPhotoId === card.photoId
                        && selectedRepresentationId === card.representationId)
                    clearPrimaryPhoto()
                return
            }
        }
        updated[key] = {
            "photoId": String(card.photoId),
            "representationId": String(card.representationId),
            "sourcePath": String(card.sourcePath),
            "title": String(card.title)
        }
        selectedPhotoTargets = updated
        selectionAnchorPhotoId = card.photoId
        selectionAnchorRepresentationId = card.representationId
        updatePrimaryPhoto(card)
    }

    function clearPrimaryPhoto() {
        selectedPhotoId = ""
        selectedRepresentationId = ""
        selectedVisualHandle = ""
        selectedDecisionHeadSequence = 0
        selectedDecisionFlag = "unflagged"
        selectedDecisionRating = 0
        selectedLiked = false
        selectedColorLabel = "none"
        selectedTitle = ""
        selectedPath = ""
        selectedSourceAvailable = true
        selectedRole = ""
        selectedVisualSource = ""
        selectedWidth = 0
        selectedHeight = 0
        controller.clearPhotoInspection()
        primaryContextInvalidated()
    }

    function clearSelection() {
        selectedPhotoTargets = ({})
        selectionAnchorPhotoId = ""
        selectionAnchorRepresentationId = ""
        clearPrimaryPhoto()
    }

    function applyDecisionChanged(photoId, headSequence, flag, rating) {
        if (selectedPhotoId !== photoId)
            return
        selectedDecisionHeadSequence = headSequence
        selectedDecisionFlag = flag
        selectedDecisionRating = rating
    }

    function applyColorLabelChanged(photoId, colorLabel) {
        if (selectedPhotoId === photoId)
            selectedColorLabel = colorLabel
    }

    function applyLikedChanged(photoId, liked) {
        if (selectedPhotoId === photoId)
            selectedLiked = liked
    }
}
