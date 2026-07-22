pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: review

    required property var controller
    required property var preferences
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

    readonly property color panel: Theme.panel
    readonly property color panelRaised: Theme.panelRaised
    readonly property color border: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent

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

    function selectPhoto(card) {
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

    function clearSelection() {
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
            grid.forceActiveFocus()
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
                visible: !review.compareMode
                enabled: visible
                focus: visible
                model: review.controller.model
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

                                Label {
                                    anchors.right: parent.right
                                    text: card.visualRole.length > 0
                                        ? card.visualRole.toUpperCase() : "RAW"
                                    color: card.visualRole === "embedded"
                                        ? Theme.successTextMuted : review.accent
                                    font.pixelSize: 8
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0.8
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
                        : qsTr("Add a folder to the local Library.\nShadow will use embedded previews first and generate a local proxy only when needed.")
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

            Item {
                id: comparisonView
                anchors.fill: parent
                anchors.margins: 18
                visible: review.compareMode

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                text: qsTr("COMPARE EVIDENCE")
                                color: review.textPrimary
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1.1
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("A local preference event, not a rank or an AI score.")
                                color: review.textMuted
                                font.pixelSize: 10
                            }
                        }

                        ShadowIconButton {
                            source: "qrc:/icons/clear.svg"
                            toolTipText: qsTr("Exit comparison (Esc)")
                            accessibleName: toolTipText
                            enabled: !review.controller.comparisonBusy
                            onClicked: review.exitComparison()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 12

                        Repeater {
                            model: [review.leftComparisonSnapshot,
                                    review.rightComparisonSnapshot]

                            delegate: Rectangle {
                                id: comparisonCard

                                required property int index
                                required property var modelData

                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredWidth: 1
                                radius: 5
                                color: review.panelRaised
                                border.color: review.border

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 8

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Label {
                                            text: comparisonCard.index === 0
                                                ? qsTr("LEFT · A") : qsTr("RIGHT · B")
                                            color: review.accent
                                            font.pixelSize: 9
                                            font.weight: Font.Bold
                                            font.letterSpacing: 1.0
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: comparisonCard.modelData
                                                ? comparisonCard.modelData.title : ""
                                            color: review.textPrimary
                                            horizontalAlignment: Text.AlignRight
                                            elide: Text.ElideMiddle
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Layout.minimumHeight: 180
                                        color: Theme.comparisonCanvas
                                        border.color: Theme.imageBorder

                                        Image {
                                            id: comparisonImage
                                            anchors.fill: parent
                                            anchors.margins: 1
                                            source: comparisonCard.index === 0
                                                ? review.leftComparisonSource
                                                : review.rightComparisonSource
                                            fillMode: Image.PreserveAspectFit
                                            asynchronous: true
                                            cache: false
                                            sourceSize.width: 1280
                                            sourceSize.height: 960
                                            onStatusChanged: {
                                                const current = comparisonCard.index === 0
                                                    ? review.leftComparisonSnapshot
                                                    : review.rightComparisonSnapshot
                                                if (!review.sameComparisonIdentity(
                                                        current,
                                                        comparisonCard.modelData)
                                                        || String(comparisonImage.source)
                                                            !== (comparisonCard.index === 0
                                                                ? review.leftComparisonSource
                                                                : review.rightComparisonSource))
                                                    return
                                                if (comparisonCard.index === 0)
                                                    review.leftComparisonVisualReady = status === Image.Ready
                                                else
                                                    review.rightComparisonVisualReady = status === Image.Ready
                                                Qt.callLater(review.refreshComparisonReadiness)
                                            }
                                        }

                                        Label {
                                            anchors.centerIn: parent
                                            visible: comparisonImage.status !== Image.Ready
                                            text: comparisonImage.status === Image.Error
                                                ? qsTr("VISUAL LOAD FAILED")
                                                : qsTr("LOADING VERIFIED VISUAL…")
                                            color: comparisonImage.status === Image.Error
                                                ? Theme.errorText : review.textMuted
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Label {
                                            text: comparisonCard.modelData
                                                && comparisonCard.modelData.visualRole.length > 0
                                                ? comparisonCard.modelData.visualRole.toUpperCase()
                                                : qsTr("DISPLAY PROXY")
                                            color: review.textMuted
                                            font.pixelSize: 8
                                            font.weight: Font.Bold
                                            font.letterSpacing: 0.8
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: comparisonCard.modelData
                                                && comparisonCard.modelData.visualWidth > 0
                                                ? qsTr("%L1 × %L2")
                                                    .arg(comparisonCard.modelData.visualWidth)
                                                    .arg(comparisonCard.modelData.visualHeight)
                                                : ""
                                            color: review.textMuted
                                            horizontalAlignment: Text.AlignRight
                                            font.pixelSize: 9
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: review.border
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("TECHNICAL · DISPLAY PROXY")
                                        color: review.textMuted
                                        font.pixelSize: 9
                                        font.weight: Font.DemiBold
                                        font.letterSpacing: 0.8
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && !comparisonCard.modelData.hasTechnicalObservation
                                        text: qsTr("No observation recorded — visual comparison is still available.")
                                        color: Theme.textQuiet
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 9
                                    }

                                    GridLayout {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        columns: 4
                                        rowSpacing: 4
                                        columnSpacing: 8

                                        Label { text: qsTr("MEAN"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.meanLuma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("P50"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.p50Luma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("P01"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.p01Luma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("P99"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.p99Luma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("BLACK"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatPercent(
                                                    comparisonCard.modelData.nearBlackFraction) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("WHITE"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatPercent(
                                                    comparisonCard.modelData.nearWhiteFraction) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("LAPL."); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatProxyDetail(
                                                    comparisonCard.modelData.laplacianVariance) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("EDGE"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatProxyDetail(
                                                    comparisonCard.modelData.edgeEnergy) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        text: comparisonCard.modelData
                                            ? qsTr("INPUT  %L1 × %L2")
                                                .arg(comparisonCard.modelData.technicalInputWidth)
                                                .arg(comparisonCard.modelData.technicalInputHeight)
                                            : ""
                                        color: Theme.textSubtle
                                        font.pixelSize: 8
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        text: comparisonCard.modelData
                                            ? qsTr("PIPELINE  %1").arg(
                                                review.concisePreprocessingVersion(
                                                    comparisonCard.modelData.technicalPreprocessingVersion))
                                            : ""
                                        color: Theme.textSubtle
                                        elide: Text.ElideRight
                                        font.pixelSize: 8
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        text: comparisonCard.modelData
                                            ? qsTr("ANALYZER  %1").arg(
                                                comparisonCard.modelData.technicalImplementationVersion)
                                            : ""
                                        color: Theme.textSubtle
                                        elide: Text.ElideRight
                                        font.pixelSize: 8
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: review.comparisonBackendReady
                            ? qsTr("EXACT ARTIFACTS + DECODED %1 FRAMES VERIFIED")
                                .arg("RGBA")
                            : qsTr("WAITING FOR BOTH EXACT COMPARE FRAME RECEIPTS")
                        color: review.comparisonBackendReady
                            ? Theme.readyText : Theme.textPending
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.7
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Feature models and ranking are not enabled. Technical facts come from each display proxy; different proxy upstreams may not be directly comparable.")
                        color: Theme.warningNoticeText
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: 9
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Repeater {
                            model: ListModel {
                                ListElement { actionId: "left-preferred"; outcomeValue: 0 }
                                ListElement { actionId: "right-preferred"; outcomeValue: 1 }
                                ListElement { actionId: "keep-both"; outcomeValue: 2 }
                                ListElement { actionId: "keep-neither"; outcomeValue: 3 }
                                ListElement { actionId: "cannot-compare"; outcomeValue: 4 }
                            }

                            delegate: ShadowButton {
                                id: outcomeButton

                                required property string actionId
                                required property int outcomeValue

                                Layout.fillWidth: true
                                Layout.preferredHeight: 36
                                variant: ShadowButton.Tinted
                                enabled: review.canSubmitComparison
                                text: {
                                    switch (outcomeButton.actionId) {
                                    case "left-preferred":
                                        return qsTr("1 · LEFT PREFERRED")
                                    case "right-preferred":
                                        return qsTr("2 · RIGHT PREFERRED")
                                    case "keep-both":
                                        return qsTr("3 · KEEP BOTH")
                                    case "keep-neither":
                                        return qsTr("4 · KEEP NEITHER")
                                    case "cannot-compare":
                                        return qsTr("0 · CANNOT COMPARE")
                                    default:
                                        return ""
                                    }
                                }
                                onClicked: review.submitComparison(outcomeValue)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        BusyIndicator {
                            visible: review.controller.comparisonBusy
                            running: visible
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 22
                        }

                        Label {
                            Layout.fillWidth: true
                            text: review.localComparisonStatus.length > 0
                                ? review.localComparisonStatus
                                : review.controller.comparisonStatusText
                            color: review.controller.comparisonBusy
                                ? review.accent : review.textMuted
                            elide: Text.ElideRight
                            font.pixelSize: 9
                        }
                    }
                }
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

        Rectangle {
            Layout.preferredWidth: 278
            Layout.fillHeight: true
            color: review.panel

            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: 1
                color: review.border
            }

            ScrollView {
                id: photoInspectorScroll

                anchors.fill: parent
                anchors.leftMargin: Theme.panelPadding + 1
                anchors.rightMargin: Theme.panelPadding
                anchors.topMargin: Theme.panelPadding
                anchors.bottomMargin: Theme.panelPadding
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: photoInspectorScroll.availableWidth
                    spacing: 10

                Label {
                    text: qsTr("PHOTO")
                    color: review.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.6
                }

                Label {
                    Layout.fillWidth: true
                    text: review.selectedTitle.length > 0
                        ? review.selectedTitle : qsTr("Nothing selected")
                    color: review.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: review.selectedPath
                    color: review.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.NoWrap
                    maximumLineCount: 1
                    elide: Text.ElideMiddle
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: review.border
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: review.selectedRole.length > 0
                            ? review.selectedRole.toUpperCase()
                            : qsTr("PENDING")
                        color: review.textPrimary
                        font.pixelSize: 10
                        font.weight: Font.Medium
                    }

                    Rectangle {
                        Layout.preferredWidth: 3
                        Layout.preferredHeight: 3
                        radius: 1.5
                        color: review.textMuted
                    }

                    Label {
                        Layout.fillWidth: true
                        text: review.selectedWidth > 0
                            ? qsTr("%L1 × %L2").arg(review.selectedWidth)
                                .arg(review.selectedHeight)
                            : "—"
                        color: review.textMuted
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: review.border
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    visible: review.selectedPhotoId.length > 0
                    color: review.border
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: review.selectedPhotoId.length > 0
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            text: qsTr("EXIF")
                            color: review.textMuted
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.2
                        }

                        Item { Layout.fillWidth: true }

                        ShadowIconButton {
                            source: "qrc:/icons/metadata.svg"
                            iconSize: 15
                            toolTipText: qsTr("View all photo metadata")
                            accessibleName: toolTipText
                            enabled: review.selectedPhotoId.length > 0
                            onClicked: metadataWindow.present()
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: !review.selectedHasMetadata
                        text: review.controller.scanning || review.controller.refreshing
                            ? qsTr("Metadata is being prepared")
                            : qsTr("No metadata is available for this photo")
                        color: Theme.textQuiet
                        font.pixelSize: 10
                    }

                    Repeater {
                        model: [
                            { id: "captured_at", label: qsTr("CAPTURED") },
                            { id: "camera", label: qsTr("CAMERA") },
                            { id: "lens", label: qsTr("LENS") },
                            { id: "exposure", label: qsTr("SHUTTER") },
                            { id: "aperture", label: qsTr("APERTURE") },
                            { id: "iso", label: qsTr("SENSITIVITY") },
                            { id: "focal_length", label: qsTr("FOCAL LENGTH") },
                            { id: "dimensions", label: qsTr("PREVIEW") },
                            { id: "focal_length_35mm", label: qsTr("35 MM EQUIV.") },
                            { id: "raw_dimensions", label: qsTr("RAW SIZE") },
                            { id: "sensor_bits", label: qsTr("BIT DEPTH") },
                            { id: "cfa", label: qsTr("CFA") },
                            { id: "dng", label: qsTr("DNG") }
                        ]

                        delegate: RowLayout {
                            id: exifRow
                            required property var modelData
                            Layout.fillWidth: true
                            visible: review.selectedHasMetadata
                                && review.preferences.exifFields.indexOf(modelData.id) >= 0
                            spacing: 8

                            Label {
                                Layout.preferredWidth: 76
                                horizontalAlignment: Text.AlignRight
                                text: exifRow.modelData.label
                                color: review.textMuted
                                font.pixelSize: 9
                            }

                            Label {
                                Layout.fillWidth: true
                                text: review.exifValue(exifRow.modelData.id)
                                color: review.textPrimary
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    visible: review.selectedPhotoId.length > 0
                    color: review.border
                }

                ColumnLayout {
                    id: compareSection

                    Layout.fillWidth: true
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("COMPARE SLOTS")
                            color: review.textMuted
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                        }

                        ShadowIconButton {
                            id: compareButton
                            source: "qrc:/icons/compare.svg"
                            iconSize: 17
                            variant: ShadowIconButton.Tinted
                            toolTipText: qsTr("Compare slots A and B")
                            accessibleName: toolTipText
                            enabled: review.comparisonReady
                                && !review.compareMode
                                && !review.controller.comparisonBusy
                                && !review.controller.decisionBusy
                                && !review.controller.scanning
                                && !review.controller.refreshing
                                && !review.controller.busy
                                && !review.controller.loadingMore
                            onClicked: review.enterComparison()
                        }

                        ShadowIconButton {
                            source: "qrc:/icons/clear.svg"
                            iconSize: 16
                            toolTipText: qsTr("Clear comparison slots")
                            accessibleName: toolTipText
                            enabled: !review.controller.comparisonBusy
                                && !review.controller.decisionBusy
                                && !review.controller.scanning
                                && !review.controller.refreshing
                                && !review.controller.busy
                                && !review.controller.loadingMore
                                && (review.leftComparisonSnapshot !== null
                                    || review.rightComparisonSnapshot !== null)
                            onClicked: review.clearComparisonSlots()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        radius: 6
                        color: Theme.surfaceSubtle

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 3
                            spacing: 6

                            Label {
                                Layout.fillWidth: true
                                text: review.leftComparisonSnapshot
                                    ? qsTr("A  %1").arg(
                                        review.leftComparisonSnapshot.title)
                                    : qsTr("A  Not set")
                                color: review.leftComparisonSnapshot
                                    ? review.textPrimary : review.textMuted
                                elide: Text.ElideMiddle
                                font.pixelSize: 10
                                font.weight: Font.Medium
                            }

                            ShadowIconButton {
                                id: setLeftButton
                                buttonSize: 28
                                source: "qrc:/icons/slot-left.svg"
                                toolTipText: qsTr("Set selected photo as comparison slot A")
                                accessibleName: toolTipText
                                enabled: !review.compareMode
                                    && !review.controller.comparisonBusy
                                    && !review.controller.decisionBusy
                                    && !review.controller.scanning
                                    && !review.controller.refreshing
                                    && !review.controller.busy
                                    && !review.controller.loadingMore
                                    && review.selectedPhotoId.length > 0
                                    && review.selectedRepresentationId.length > 0
                                    && review.selectedVisualHandle.length > 0
                                    && review.selectedVisualSource.length > 0
                                    && (review.rightComparisonSnapshot === null
                                        || review.rightComparisonSnapshot.photoId
                                            !== review.selectedPhotoId)
                                onClicked: review.setSelectedAsLeft()
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        radius: 6
                        color: Theme.surfaceSubtle

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 3
                            spacing: 6

                            Label {
                                Layout.fillWidth: true
                                text: review.rightComparisonSnapshot
                                    ? qsTr("B  %1").arg(
                                        review.rightComparisonSnapshot.title)
                                    : qsTr("B  Not set")
                                color: review.rightComparisonSnapshot
                                    ? review.textPrimary : review.textMuted
                                elide: Text.ElideMiddle
                                font.pixelSize: 10
                                font.weight: Font.Medium
                            }

                            ShadowIconButton {
                                id: setRightButton
                                buttonSize: 28
                                source: "qrc:/icons/slot-right.svg"
                                toolTipText: qsTr("Set selected photo as comparison slot B")
                                accessibleName: toolTipText
                                enabled: !review.compareMode
                                    && !review.controller.comparisonBusy
                                    && !review.controller.decisionBusy
                                    && !review.controller.scanning
                                    && !review.controller.refreshing
                                    && !review.controller.busy
                                    && !review.controller.loadingMore
                                    && review.selectedPhotoId.length > 0
                                    && review.selectedRepresentationId.length > 0
                                    && review.selectedVisualHandle.length > 0
                                    && review.selectedVisualSource.length > 0
                                    && (review.leftComparisonSnapshot === null
                                        || review.leftComparisonSnapshot.photoId
                                            !== review.selectedPhotoId)
                                onClicked: review.setSelectedAsRight()
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: review.selectedPhotoId.length > 0
                            && review.selectedVisualSource.length === 0
                        text: qsTr("A display visual is required for comparison.")
                        color: Theme.warningNoticeText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 10
                    }

                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: review.border
                }

                Label {
                    Layout.fillWidth: true
                    visible: review.precisionOpenStatus.length > 0
                    text: review.precisionOpenStatus
                    color: Theme.warningText
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontMeta
                }

                }
            }
        }
    }
}
