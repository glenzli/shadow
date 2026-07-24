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
    property var selectedPhotoTargets: ({})
    property string selectedPhotoId: ""
    property string selectedRepresentationId: ""
    property string selectedVisualHandle: ""
    property var selectedDecisionHeadSequence: 0
    property string selectedDecisionFlag: "unflagged"
    property int selectedDecisionRating: 0
    property string selectedColorLabel: "none"
    property string selectedTitle: ""
    property string selectedPath: ""
    property string selectedRole: ""
    property string selectedVisualSource: ""
    property int selectedWidth: 0
    property int selectedHeight: 0
    property bool selectedHasMetadata: false
    property string selectedCameraMake: ""
    property string selectedCameraModel: ""
    property string selectedLensMake: ""
    property string selectedLensModel: ""
    property var selectedCapturedAtUnixSeconds: 0
    property real selectedIsoSpeed: 0.0
    property real selectedExposureTimeSeconds: 0.0
    property real selectedApertureFNumber: 0.0
    property real selectedFocalLengthMm: 0.0
    property real selectedFocalLength35mm: 0.0
    property int selectedRawWidth: 0
    property int selectedRawHeight: 0
    property int selectedSensorBits: 0
    property string selectedCfaPattern: ""
    property string selectedDngVersion: ""
    property bool selectedHasTechnicalObservation: false
    property int selectedTechnicalInputWidth: 0
    property int selectedTechnicalInputHeight: 0
    property string selectedTechnicalPreprocessingVersion: ""
    property string selectedTechnicalImplementationVersion: ""
    property real selectedMeanLuma: 0.0
    property real selectedP01Luma: 0.0
    property real selectedP50Luma: 0.0
    property real selectedP99Luma: 0.0
    property real selectedNearBlackFraction: 0.0
    property real selectedNearWhiteFraction: 0.0
    property real selectedLaplacianVariance: 0.0
    property real selectedEdgeEnergy: 0.0
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
    readonly property int selectedPhotoCount:
        Object.keys(selectedPhotoTargets).length
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
        metadataPending: review.controller.scanning || review.controller.refreshing
        fields: review.metadataFields()
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

            Repeater {
                model: review.controller.sharedGradeNodes

                delegate: Rectangle {
                    id: sharedBatchRow
                    required property var modelData
                    width: parent.width
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
        if (selectedPhotoId !== card.photoId
                || selectedRepresentationId !== card.representationId)
            precisionOpenStatus = ""
        selectedPhotoId = card.photoId
        selectedRepresentationId = card.representationId
        selectedVisualHandle = card.visualHandle
        selectedDecisionHeadSequence = card.decisionHeadSequence
        selectedDecisionFlag = card.decisionFlag
        selectedDecisionRating = card.decisionRating
        selectedColorLabel = card.colorLabel
        selectedTitle = card.title
        selectedPath = card.sourcePath
        selectedRole = card.visualRole
        selectedVisualSource = card.visualSource
        selectedWidth = card.visualWidth
        selectedHeight = card.visualHeight
        selectedHasMetadata = card.hasMetadata
        selectedCameraMake = card.cameraMake
        selectedCameraModel = card.cameraModel
        selectedLensMake = card.lensMake
        selectedLensModel = card.lensModel
        selectedCapturedAtUnixSeconds = card.capturedAtUnixSeconds
        selectedIsoSpeed = card.isoSpeed
        selectedExposureTimeSeconds = card.exposureTimeSeconds
        selectedApertureFNumber = card.apertureFNumber
        selectedFocalLengthMm = card.focalLengthMm
        selectedFocalLength35mm = card.focalLength35mm
        selectedRawWidth = card.rawWidth
        selectedRawHeight = card.rawHeight
        selectedSensorBits = card.sensorBits
        selectedCfaPattern = card.cfaPattern
        selectedDngVersion = card.dngVersion
        selectedHasTechnicalObservation = card.hasTechnicalObservation
        selectedTechnicalInputWidth = card.technicalInputWidth
        selectedTechnicalInputHeight = card.technicalInputHeight
        selectedTechnicalPreprocessingVersion = card.technicalPreprocessingVersion
        selectedTechnicalImplementationVersion = card.technicalImplementationVersion
        selectedMeanLuma = card.meanLuma
        selectedP01Luma = card.p01Luma
        selectedP50Luma = card.p50Luma
        selectedP99Luma = card.p99Luma
        selectedNearBlackFraction = card.nearBlackFraction
        selectedNearWhiteFraction = card.nearWhiteFraction
        selectedLaplacianVariance = card.laplacianVariance
        selectedEdgeEnergy = card.edgeEnergy
    }

    function selectPhoto(card, modifiers) {
        const modifierMask = Number(modifiers || 0)
        const additive = (modifierMask & Qt.ControlModifier) !== 0
            || (modifierMask & Qt.MetaModifier) !== 0
        const key = selectionKey(card.photoId, card.representationId)
        const updated = ({})
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
        updatePrimaryPhoto(card)
    }

    function clearPrimaryPhoto() {
        precisionOpenStatus = ""
        selectedPhotoId = ""
        selectedRepresentationId = ""
        selectedVisualHandle = ""
        selectedDecisionHeadSequence = 0
        selectedDecisionFlag = "unflagged"
        selectedDecisionRating = 0
        selectedColorLabel = "none"
        selectedTitle = ""
        selectedPath = ""
        selectedRole = ""
        selectedVisualSource = ""
        selectedWidth = 0
        selectedHeight = 0
        selectedHasMetadata = false
        selectedCameraMake = ""
        selectedCameraModel = ""
        selectedLensMake = ""
        selectedLensModel = ""
        selectedCapturedAtUnixSeconds = 0
        selectedIsoSpeed = 0
        selectedExposureTimeSeconds = 0
        selectedApertureFNumber = 0
        selectedFocalLengthMm = 0
        selectedFocalLength35mm = 0
        selectedRawWidth = 0
        selectedRawHeight = 0
        selectedSensorBits = 0
        selectedCfaPattern = ""
        selectedDngVersion = ""
        selectedHasTechnicalObservation = false
        selectedTechnicalInputWidth = 0
        selectedTechnicalInputHeight = 0
        selectedTechnicalPreprocessingVersion = ""
        selectedTechnicalImplementationVersion = ""
        selectedMeanLuma = 0.0
        selectedP01Luma = 0.0
        selectedP50Luma = 0.0
        selectedP99Luma = 0.0
        selectedNearBlackFraction = 0.0
        selectedNearWhiteFraction = 0.0
        selectedLaplacianVariance = 0.0
        selectedEdgeEnergy = 0.0
    }

    function clearSelection() {
        selectedPhotoTargets = ({})
        clearPrimaryPhoto()
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
            if (review.selectedPhotoId === photoId) {
                review.selectedDecisionHeadSequence = headSequence
                review.selectedDecisionFlag = flag
                review.selectedDecisionRating = rating
            }
        }
        function onColorLabelChanged(photoId, colorLabel) {
            if (review.selectedPhotoId === photoId)
                review.selectedColorLabel = colorLabel
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
                    color: Theme.accentSurface

                    Rectangle {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 3
                        width: 3
                        height: 18
                        radius: 1.5
                        color: review.accent
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
                        text: qsTr("ALL PHOTOS")
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

                    Item { Layout.fillWidth: true }

                    ShadowIcon {
                        source: "qrc:/icons/review-grid.svg"
                        color: review.accent
                        size: 17
                    }

                    Label {
                        text: qsTr("SCALE")
                        color: review.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.7
                    }

                    ShadowInlineSlider {
                        id: galleryScaleSlider
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

            // Kept temporarily as a non-instantiated source-level reference
            // while the justified ListView below owns the visible gallery. It
            // prevents any legacy crop delegate from requesting images.
            GridView {
                id: grid
                objectName: "reviewGrid"

                function maybeLoadMore() {
                    if (review.controller.hasMore
                            && !review.controller.scanning
                            && !review.controller.refreshing
                            && !review.controller.busy
                            && !review.controller.loadingMore
                            && contentY + height >= contentHeight - cellHeight * 2) {
                        review.controller.loadMore()
                    }
                }

                anchors.fill: parent
                anchors.margins: 18
                clip: true
                visible: false
                enabled: false
                focus: false
                model: null
                cellWidth: Math.max(220, Math.floor(width / Math.max(1, Math.floor(width / 270))))
                cellHeight: cellWidth * 0.78
                currentIndex: -1
                onContentYChanged: maybeLoadMore()
                onHeightChanged: Qt.callLater(maybeLoadMore)
                onCountChanged: {
                    if (count === 0)
                        currentIndex = -1
                    else if (currentIndex < 0)
                        currentIndex = 0
                    Qt.callLater(maybeLoadMore)
                }
                onCurrentItemChanged: {
                    if (currentItem)
                        review.selectPhoto(currentItem)
                }

                delegate: Item {
                    id: card
                    width: grid.cellWidth
                    height: grid.cellHeight

                    required property int index
                    required property string photoId
                    required property string representationId
                    required property string visualHandle
                    required property string title
                    required property string sourcePath
                    required property string visualRole
                    required property string visualError
                    required property int visualWidth
                    required property int visualHeight
                    required property string visualSource
                    required property bool hasMetadata
                    required property string cameraMake
                    required property string cameraModel
                    required property string lensMake
                    required property string lensModel
                    required property var capturedAtUnixSeconds
                    required property real isoSpeed
                    required property real exposureTimeSeconds
                    required property real apertureFNumber
                    required property real focalLengthMm
                    required property real focalLength35mm
                    required property int rawWidth
                    required property int rawHeight
                    required property int sensorBits
                    required property string cfaPattern
                    required property string dngVersion
                    required property var decisionHeadSequence
                    required property string decisionFlag
                    required property int decisionRating
                    required property string colorLabel
                    required property bool hasDevelopmentEdits
                    required property bool hasTechnicalObservation
                    required property int technicalInputWidth
                    required property int technicalInputHeight
                    required property string technicalPreprocessingVersion
                    required property string technicalImplementationVersion
                    required property real meanLuma
                    required property real p01Luma
                    required property real p50Luma
                    required property real p99Luma
                    required property real nearBlackFraction
                    required property real nearWhiteFraction
                    required property real laplacianVariance
                    required property real edgeEnergy
                    readonly property bool selected:
                        review.selectedPhotoId === card.photoId
                            && review.selectedRepresentationId
                                === card.representationId

                    function refreshSelectedMetadata() {
                        if (!card.selected)
                            return
                        Qt.callLater(() => {
                            if (card.selected)
                                review.selectPhoto(card)
                        })
                    }

                    // A final Library snapshot may replace a delegate while
                    // preserving the same selected photo id. Its metadata
                    // properties are then initialized before `selected`
                    // becomes true, so none of the field change handlers below
                    // can refresh the right panel. Reconcile once selection is
                    // established as well.
                    onSelectedChanged: refreshSelectedMetadata()
                    onHasMetadataChanged: refreshSelectedMetadata()
                    onCameraMakeChanged: refreshSelectedMetadata()
                    onCameraModelChanged: refreshSelectedMetadata()
                    onLensMakeChanged: refreshSelectedMetadata()
                    onLensModelChanged: refreshSelectedMetadata()
                    onIsoSpeedChanged: refreshSelectedMetadata()
                    onExposureTimeSecondsChanged: refreshSelectedMetadata()
                    onApertureFNumberChanged: refreshSelectedMetadata()
                    onFocalLengthMmChanged: refreshSelectedMetadata()
                    onFocalLength35mmChanged: refreshSelectedMetadata()
                    onCapturedAtUnixSecondsChanged: refreshSelectedMetadata()

                    Accessible.role: Accessible.ListItem
                    Accessible.name: card.title
                    Accessible.selected: card.selected

                    RectangularShadow {
                        x: 6
                        y: 6
                        width: parent.width - 12
                        height: parent.height - 12
                        offset: Qt.vector2d(0, 2)
                        radius: 10
                        blur: 12
                        spread: -2
                        color: Theme.shadowSoft
                        opacity: card.selected ? 0.9
                            : cardMouse.containsMouse ? 0.35 : 0.0
                        cached: true

                        Behavior on opacity {
                            NumberAnimation { duration: 120 }
                        }
                    }

                    Rectangle {
                        id: cardSurface
                        anchors.fill: parent
                        anchors.margins: 6
                        radius: 10
                        clip: true
                        color: review.panelRaised
                        border.width: 1
                        border.color: card.selected
                            ? Theme.accentBorder : review.border
                        scale: cardMouse.pressed ? 0.995 : 1.0

                        Behavior on scale {
                            NumberAnimation { duration: 80 }
                        }

                        Image {
                            id: thumbnail
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: caption.top
                            source: card.visualSource
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: true
                            sourceSize.width: 640
                            sourceSize.height: 480
                        }

                        Rectangle {
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.topMargin: 14
                            anchors.leftMargin: 14
                            width: 10
                            height: 10
                            radius: width / 2
                            visible: card.colorLabel !== "none"
                            color: Theme.colorLabel(card.colorLabel)
                        }

                        Rectangle {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.topMargin: 12
                            anchors.rightMargin: 12
                            width: 26
                            height: 26
                            radius: Theme.compactControlRadius
                            visible: card.decisionFlag !== "unflagged"
                            color: card.decisionFlag === "picked"
                                ? Theme.successSurface : Theme.dangerSurface
                            border.color: card.decisionFlag === "picked"
                                ? Theme.successBorder : Theme.dangerBorder

                            ShadowIcon {
                                anchors.centerIn: parent
                                source: card.decisionFlag === "picked"
                                    ? "qrc:/icons/pick.svg"
                                    : "qrc:/icons/reject.svg"
                                color: card.decisionFlag === "picked"
                                    ? Theme.successText : Theme.dangerText
                                size: 15
                            }
                        }

                        Rectangle {
                            anchors.fill: thumbnail
                            visible: card.visualSource.length === 0
                            color: Theme.surfaceSubtle

                            Column {
                                anchors.centerIn: parent
                                spacing: 8
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "RAW"
                                    color: Theme.rawPlaceholderText
                                    font.pixelSize: 20
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 2
                                }
                                Label {
                                    text: card.visualError.length > 0
                                        ? qsTr("PREVIEW PENDING") : qsTr("NO VISUAL")
                                    color: review.textMuted
                                    font.pixelSize: 9
                                }
                            }
                        }

                        Rectangle {
                            id: caption
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 52
                            color: card.selected
                                ? Theme.accentSelectionSurface
                                : Theme.thumbnailCaptionOverlay

                            Rectangle {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 3
                                height: 28
                                radius: 1.5
                                visible: card.selected
                                color: review.accent
                            }

                            Column {
                                anchors.left: parent.left
                                anchors.right: cardMetadata.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 10
                                spacing: 3
                                Label {
                                    width: parent.width
                                    text: card.title
                                    color: review.textPrimary
                                    elide: Text.ElideRight
                                    font.pixelSize: 11
                                }
                                Label {
                                    text: card.visualWidth > 0
                                        ? qsTr("%L1 × %L2").arg(card.visualWidth)
                                            .arg(card.visualHeight)
                                        : qsTr("awaiting cache")
                                    color: review.textMuted
                                    font.pixelSize: 9
                                }
                            }

                            Column {
                                id: cardMetadata
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3

                                ShadowIcon {
                                    anchors.right: parent.right
                                    visible: card.hasDevelopmentEdits
                                    source: "qrc:/icons/edit.svg"
                                    color: review.accent
                                    size: 12
                                }

                                Row {
                                    anchors.right: parent.right
                                    visible: card.decisionRating > 0
                                    spacing: 1

                                    Repeater {
                                        model: card.decisionRating

                                        ShadowIcon {
                                            required property int index
                                            source: "qrc:/icons/star-filled.svg"
                                            color: review.accent
                                            size: 8
                                        }
                                    }
                                }
                            }
                        }

                        MouseArea {
                            id: cardMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                grid.currentIndex = card.index
                                review.selectPhoto(card)
                            }
                            onDoubleClicked: {
                                grid.currentIndex = card.index
                                review.selectPhoto(card)
                                review.openSelectedPhoto()
                            }
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        z: 2
                        visible: card.selected
                        radius: 12
                        color: Theme.transparent
                        border.width: 3
                        border.color: review.accent
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: Math.min(420, parent.width - 60)
                    visible: grid.count === 0 && !review.controller.busy
                    text: review.controller.scanning
                        ? qsTr("Searching the folder for supported photos…\nNew RAW files will appear here as they are catalogued.")
                        : review.controller.scanProgress.phase === "failed"
                        ? qsTr("Import stopped, and no RAW files are currently visible.\nAlready catalogued files remain safely stored.")
                        : qsTr("Add a folder to the local Library.\nShadow will show embedded previews immediately, then replace them with locally generated proxies.")
                    color: review.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    lineHeight: 1.4
                }

                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 18
                    visible: review.controller.loadingMore
                    running: visible
                    width: 34
                    height: 34
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

            Label {
                anchors.centerIn: justifiedGrid
                width: Math.min(420, justifiedGrid.width - 60)
                visible: justifiedGrid.count === 0 && !review.controller.busy
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
