pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick

// Ordinary comparison is a non-mutating two-photo workspace. Each pane owns
// an independent navigation cursor and consumes repeatable Library visuals;
// exact one-purpose evidence tickets belong only to explicit feedback capture.
QtObject {
    id: comparison

    required property var controller
    required property var selection
    required property var navigationModel

    property var leftComparisonSnapshot: null
    property var rightComparisonSnapshot: null
    property bool compareMode: false
    property var groupSnapshots: []
    readonly property bool groupMode: groupSnapshots.length > 1
    readonly property int groupCount: groupSnapshots.length
    property string localComparisonStatusKey: ""
    property int localComparisonStatusSlot: -1

    readonly property bool comparisonReady:
        snapshotReady(leftComparisonSnapshot)
        && snapshotReady(rightComparisonSnapshot)
    readonly property string leftComparisonSource:
        leftComparisonSnapshot === null
            ? "" : String(leftComparisonSnapshot.visualSource || "")
    readonly property string rightComparisonSource:
        rightComparisonSnapshot === null
            ? "" : String(rightComparisonSnapshot.visualSource || "")
    readonly property string localComparisonStatus: {
        if (localComparisonStatusKey === "photo-in-both-slots")
            return qsTr("A photo cannot occupy both comparison panes.")
        if (localComparisonStatusKey === "no-adjacent-photo")
            return qsTr("No other photo is available in the current view.")
        return ""
    }

    function normalizedSnapshot(value) {
        if (!value)
            return null
        return {
            "photoId": String(value.photoId || ""),
            "representationId": String(value.representationId || ""),
            "visualHandle": String(value.visualHandle || ""),
            "title": String(value.title || ""),
            "sourcePath": String(value.sourcePath || ""),
            "sourceAvailable": value.sourceAvailable === undefined
                ? true : Boolean(value.sourceAvailable),
            "isRemote": Boolean(value.isRemote),
            "remoteOriginalCached": Boolean(value.remoteOriginalCached),
            "visualRole": String(value.visualRole || ""),
            "visualSource": String(value.visualSource || ""),
            "visualWidth": Number(value.visualWidth || 0),
            "visualHeight": Number(value.visualHeight || 0)
        }
    }

    function selectedSnapshot() {
        if (typeof selection.selectedSnapshot === "function")
            return normalizedSnapshot(selection.selectedSnapshot())
        return normalizedSnapshot({
            "photoId": selection.selectedPhotoId,
            "representationId": selection.selectedRepresentationId,
            "visualHandle": selection.selectedVisualHandle,
            "title": selection.selectedTitle,
            "sourcePath": selection.selectedPath,
            "visualRole": selection.selectedRole,
            "visualSource": selection.selectedVisualSource,
            "visualWidth": selection.selectedWidth,
            "visualHeight": selection.selectedHeight
        })
    }

    function snapshotReady(snapshot) {
        return snapshot !== null
            && String(snapshot.photoId || "").length > 0
            && String(snapshot.representationId || "").length > 0
            && String(snapshot.visualSource || "").length > 0
    }

    function sameIdentity(left, right) {
        return left !== null && right !== null
            && String(left.photoId || "") === String(right.photoId || "")
            && String(left.representationId || "")
                === String(right.representationId || "")
    }

    function selectionCanFillPane() {
        return snapshotReady(selectedSnapshot())
    }

    function clearLocalComparisonStatus() {
        localComparisonStatusKey = ""
        localComparisonStatusSlot = -1
    }

    function setSelectedAsLeft() {
        const snapshot = selectedSnapshot()
        if (!snapshotReady(snapshot))
            return
        if (sameIdentity(snapshot, rightComparisonSnapshot)) {
            localComparisonStatusKey = "photo-in-both-slots"
            localComparisonStatusSlot = 0
            return
        }
        leftComparisonSnapshot = snapshot
        clearLocalComparisonStatus()
    }

    function setSelectedAsRight() {
        const snapshot = selectedSnapshot()
        if (!snapshotReady(snapshot))
            return
        if (sameIdentity(snapshot, leftComparisonSnapshot)) {
            localComparisonStatusKey = "photo-in-both-slots"
            localComparisonStatusSlot = 1
            return
        }
        rightComparisonSnapshot = snapshot
        clearLocalComparisonStatus()
    }

    function adjacentSnapshot(snapshot, direction) {
        if (!snapshotReady(snapshot))
            return null
        return normalizedSnapshot(navigationModel.navigationTarget(
            snapshot.photoId,
            snapshot.representationId,
            direction,
            0))
    }

    function groupAdjacentSnapshot(snapshot, direction, other) {
        const currentIndex = groupSnapshots.findIndex(value => sameIdentity(value, snapshot))
        if (currentIndex < 0)
            return null
        for (let step = 1; step < groupSnapshots.length; ++step) {
            const nextIndex = (currentIndex + direction * step
                + groupSnapshots.length * step) % groupSnapshots.length
            const target = groupSnapshots[nextIndex]
            if (!sameIdentity(target, other))
                return target
        }
        return null
    }

    function startGroupComparison(values) {
        const snapshots = []
        for (let index = 0; index < values.length; ++index) {
            const snapshot = normalizedSnapshot(values[index])
            if (!snapshotReady(snapshot))
                continue
            if (snapshots.some(value => sameIdentity(value, snapshot)))
                continue
            snapshots.push(snapshot)
        }
        if (snapshots.length < 2)
            return false
        groupSnapshots = snapshots
        leftComparisonSnapshot = snapshots[0]
        rightComparisonSnapshot = snapshots[1]
        compareMode = true
        clearLocalComparisonStatus()
        return true
    }

    function startQuickComparison() {
        groupSnapshots = []
        const selected = selectedSnapshot()
        if (!snapshotReady(selected))
            return
        let adjacent = adjacentSnapshot(selected, 1)
        if (!snapshotReady(adjacent))
            adjacent = adjacentSnapshot(selected, -1)
        if (!snapshotReady(adjacent)) {
            localComparisonStatusKey = "no-adjacent-photo"
            localComparisonStatusSlot = -1
            return
        }
        leftComparisonSnapshot = selected
        rightComparisonSnapshot = adjacent
        compareMode = true
        clearLocalComparisonStatus()
    }

    // This is intentionally separate from the adjacent-photo convenience
    // action. The caller supplies two immutable selection snapshots, so the
    // two panes begin exactly with the photos the user selected.
    function startSelectedComparison(leftValue, rightValue) {
        groupSnapshots = []
        const left = normalizedSnapshot(leftValue)
        const right = normalizedSnapshot(rightValue)
        if (!snapshotReady(left) || !snapshotReady(right)
                || sameIdentity(left, right))
            return false
        leftComparisonSnapshot = left
        rightComparisonSnapshot = right
        compareMode = true
        clearLocalComparisonStatus()
        return true
    }

    function enterComparison() {
        if (comparisonReady) {
            compareMode = true
            clearLocalComparisonStatus()
        }
    }

    function navigatePane(slot, direction) {
        if (!compareMode)
            return
        const current = slot === 0
            ? leftComparisonSnapshot : rightComparisonSnapshot
        const other = slot === 0
            ? rightComparisonSnapshot : leftComparisonSnapshot
        const target = groupMode
            ? groupAdjacentSnapshot(current, direction, other)
            : adjacentSnapshot(current, direction)
        if (!snapshotReady(target) || sameIdentity(target, other)) {
            localComparisonStatusKey = "no-adjacent-photo"
            localComparisonStatusSlot = slot
            return
        }
        if (slot === 0)
            leftComparisonSnapshot = target
        else
            rightComparisonSnapshot = target
        clearLocalComparisonStatus()
    }

    function swapPanes() {
        if (!comparisonReady)
            return
        const previousLeft = leftComparisonSnapshot
        leftComparisonSnapshot = rightComparisonSnapshot
        rightComparisonSnapshot = previousLeft
        clearLocalComparisonStatus()
    }

    function clearComparisonSlots() {
        groupSnapshots = []
        leftComparisonSnapshot = null
        rightComparisonSnapshot = null
        compareMode = false
        clearLocalComparisonStatus()
    }

    function exitComparison() {
        compareMode = false
        groupSnapshots = []
        clearLocalComparisonStatus()
    }

    property Connections controllerConnections: Connections {
        target: comparison.controller

        function onItemCountChanged() {
            if (comparison.controller.itemCount === 0)
                comparison.clearComparisonSlots()
        }
    }
}
