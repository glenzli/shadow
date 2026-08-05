pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick

QtObject {
    id: comparison

    required property var controller
    required property var selection
    required property var navigationModel

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
    property bool selectionCompareMode: false
    property int candidateDirection: 1
    property string pendingRecordedAction: ""
    property var nearbyCandidateTargets: []
    property string localComparisonStatusKey: ""
    property int localComparisonStatusSlot: -1

    readonly property bool comparisonReady:
        leftComparisonSnapshot !== null
        && rightComparisonSnapshot !== null
    readonly property bool comparisonVisualsReady:
        leftComparisonVisualReady && rightComparisonVisualReady
    readonly property bool canSubmitComparison:
        comparisonReady
        && comparisonVisualsReady
        && comparisonBackendReady
        && compareMode
        && !controller.scanning
        && !controller.refreshing
        && !controller.comparisonBusy
        && !controller.decisionBusy
    readonly property string localComparisonStatus: {
        if (localComparisonStatusKey === "photo-in-both-slots")
            return qsTr("A photo cannot occupy both comparison slots.")
        if (localComparisonStatusKey === "slot-updated")
            return localComparisonStatusSlot === 0
                ? qsTr("Left evidence slot updated.")
                : qsTr("Right evidence slot updated.")
        if (localComparisonStatusKey === "no-adjacent-photo")
            return qsTr("No adjacent photo is available for quick comparison.")
        return ""
    }

    signal comparisonRecorded()

    function selectedComparisonSnapshot() {
        return {
            "photoId": String(selection.selectedPhotoId),
            "representationId": String(selection.selectedRepresentationId),
            "visualHandle": String(selection.selectedVisualHandle),
            "title": String(selection.selectedTitle),
            "sourcePath": String(selection.selectedPath),
            "visualRole": String(selection.selectedRole),
            "visualSource": String(selection.selectedVisualSource),
            "visualWidth": Number(selection.selectedWidth),
            "visualHeight": Number(selection.selectedHeight),
            "hasTechnicalObservation":
                Boolean(selection.selectedHasTechnicalObservation),
            "technicalInputWidth":
                Number(selection.selectedTechnicalInputWidth),
            "technicalInputHeight":
                Number(selection.selectedTechnicalInputHeight),
            "technicalPreprocessingVersion": String(
                selection.selectedTechnicalPreprocessingVersion),
            "technicalImplementationVersion": String(
                selection.selectedTechnicalImplementationVersion),
            "meanLuma": Number(selection.selectedMeanLuma),
            "p01Luma": Number(selection.selectedP01Luma),
            "p50Luma": Number(selection.selectedP50Luma),
            "p99Luma": Number(selection.selectedP99Luma),
            "nearBlackFraction":
                Number(selection.selectedNearBlackFraction),
            "nearWhiteFraction":
                Number(selection.selectedNearWhiteFraction),
            "laplacianVariance":
                Number(selection.selectedLaplacianVariance),
            "edgeEnergy": Number(selection.selectedEdgeEnergy)
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

    function selectionCanFillSlot() {
        return selection.selectedPhotoId.length > 0
            && selection.selectedRepresentationId.length > 0
            && selection.selectedVisualHandle.length > 0
            && selection.selectedVisualSource.length > 0
    }

    function setSelectedAsLeft() {
        if (!selectionCanFillSlot())
            return
        if (rightComparisonSnapshot !== null
                && rightComparisonSnapshot.photoId
                    === selection.selectedPhotoId) {
            setLocalComparisonStatus("photo-in-both-slots", -1)
            return
        }
        leftComparisonVisualReady = false
        comparisonBackendReady = false
        leftComparisonSnapshot = selectedComparisonSnapshot()
        setLocalComparisonStatus("slot-updated", 0)
    }

    function setSelectedAsRight() {
        if (!selectionCanFillSlot())
            return
        if (leftComparisonSnapshot !== null
                && leftComparisonSnapshot.photoId
                    === selection.selectedPhotoId) {
            setLocalComparisonStatus("photo-in-both-slots", -1)
            return
        }
        rightComparisonVisualReady = false
        comparisonBackendReady = false
        rightComparisonSnapshot = selectedComparisonSnapshot()
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
        selectionCompareMode = false
        pendingRecordedAction = ""
        nearbyCandidateTargets = []
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

    function adjacentTarget(direction) {
        return navigationModel.navigationTarget(
            selection.selectedPhotoId,
            selection.selectedRepresentationId,
            direction,
            0)
    }

    function startSelectionComparison() {
        if (!selectionCanFillSlot())
            return
        leftComparisonSnapshot = selectedComparisonSnapshot()
        let target = adjacentTarget(1)
        candidateDirection = 1
        if (!target || String(target.photoId || "").length === 0) {
            target = adjacentTarget(-1)
            candidateDirection = -1
        }
        if (!target || String(target.photoId || "").length === 0) {
            setLocalComparisonStatus("no-adjacent-photo", -1)
            return
        }
        selectionCompareMode = true
        selection.selectPhoto(target, 0)
        rightComparisonSnapshot = selectedComparisonSnapshot()
        refreshNearbyCandidates()
        enterComparison()
    }

    function refreshNearbyCandidates() {
        if (!selectionCompareMode || rightComparisonSnapshot === null) {
            nearbyCandidateTargets = []
            return
        }
        const before = []
        let cursorPhotoId = selection.selectedPhotoId
        let cursorRepresentationId = selection.selectedRepresentationId
        for (let index = 0; index < 2; ++index) {
            const target = navigationModel.navigationTarget(
                cursorPhotoId, cursorRepresentationId, -1, 0)
            if (!target || String(target.photoId || "").length === 0)
                break
            cursorPhotoId = String(target.photoId)
            cursorRepresentationId = String(target.representationId)
            if (cursorPhotoId !== leftComparisonSnapshot.photoId)
                before.unshift(target)
        }
        const values = before
        values.push(rightComparisonSnapshot)
        cursorPhotoId = selection.selectedPhotoId
        cursorRepresentationId = selection.selectedRepresentationId
        for (let index = 0; index < 2; ++index) {
            const target = navigationModel.navigationTarget(
                cursorPhotoId, cursorRepresentationId, 1, 0)
            if (!target || String(target.photoId || "").length === 0)
                break
            cursorPhotoId = String(target.photoId)
            cursorRepresentationId = String(target.representationId)
            if (cursorPhotoId !== leftComparisonSnapshot.photoId)
                values.push(target)
        }
        nearbyCandidateTargets = values
    }

    function chooseCandidate(target) {
        if (!selectionCompareMode || controller.comparisonBusy || !target
                || String(target.photoId || "").length === 0
                || String(target.photoId) === leftComparisonSnapshot.photoId
                || String(target.photoId) === selection.selectedPhotoId)
            return
        resetPreparedComparison(true)
        selection.selectPhoto(target, 0)
        rightComparisonSnapshot = selectedComparisonSnapshot()
        refreshNearbyCandidates()
        enterComparison()
    }

    function navigateCandidate(direction) {
        if (!selectionCompareMode || controller.comparisonBusy)
            return
        const target = adjacentTarget(direction)
        if (!target || String(target.photoId || "").length === 0
                || String(target.photoId) === leftComparisonSnapshot.photoId) {
            setLocalComparisonStatus("no-adjacent-photo", -1)
            return
        }
        candidateDirection = direction
        resetPreparedComparison(true)
        selection.selectPhoto(target, 0)
        rightComparisonSnapshot = selectedComparisonSnapshot()
        refreshNearbyCandidates()
        enterComparison()
    }

    function submitSelectionComparison(outcome, recordedAction) {
        if (!canSubmitComparison)
            return
        pendingRecordedAction = recordedAction
        controller.recordComparison(comparisonPresentationId, outcome)
    }

    function promoteCandidate() {
        submitSelectionComparison(1, "promote")
    }

    function keepAnchor() {
        submitSelectionComparison(0, "advance")
    }

    function keepBoth() {
        submitSelectionComparison(2, "advance")
    }

    function finishSelectionComparisonRecord() {
        const promoted = pendingRecordedAction === "promote"
        pendingRecordedAction = ""
        resetPreparedComparison(false)
        if (promoted)
            leftComparisonSnapshot = rightComparisonSnapshot
        rightComparisonSnapshot = null
        comparisonRecorded()
        const target = adjacentTarget(candidateDirection)
        if (!target || String(target.photoId || "").length === 0
                || String(target.photoId) === leftComparisonSnapshot.photoId) {
            clearComparisonSlots(false)
            return
        }
        selection.selectPhoto(target, 0)
        rightComparisonSnapshot = selectedComparisonSnapshot()
        refreshNearbyCandidates()
        enterComparison()
    }

    function exitComparison() {
        resetPreparedComparison(true)
        compareMode = false
        selectionCompareMode = false
        pendingRecordedAction = ""
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
        if (canSubmitComparison)
            controller.recordComparison(comparisonPresentationId, outcome)
    }

    property Connections controllerConnections: Connections {
        target: comparison.controller

        function onItemCountChanged() {
            if (comparison.controller.itemCount === 0)
                comparison.clearComparisonSlots()
        }

        function onComparisonRecorded() {
            if (comparison.selectionCompareMode) {
                comparison.finishSelectionComparisonRecord()
                return
            }
            comparison.clearComparisonSlots(false)
            comparison.comparisonRecorded()
        }

        function onComparisonForgotten() {
            comparison.clearLocalComparisonStatus()
        }
    }
}
