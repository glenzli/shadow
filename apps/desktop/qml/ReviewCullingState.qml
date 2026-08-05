pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick

// Owns one temporary culling draft and its pairwise ranking lifecycle. The
// draft never mutates Library decisions; Like, rating, Pick, and album actions
// remain explicit operations after the user leaves the arena.
QtObject {
    id: culling

    required property var selection

    property var candidates: []
    property var arenaCandidates: []
    property var tiers: []
    property var unresolvedCandidates: []
    property var currentLeft: null
    property var currentRight: null
    property int nextCandidateIndex: 0
    property int searchLow: 0
    property int searchHigh: -1
    property int searchMid: -1
    property int comparisonCount: 0
    property var history: []
    property bool arenaActive: false
    property bool arenaComplete: false

    readonly property int candidateCount: candidates.length
    readonly property bool canStartArena: candidateCount >= 2
    readonly property int rankedCandidateCount: rankedCount()
    readonly property int totalArenaCandidateCount: arenaCandidates.length

    function normalizedSnapshot(value) {
        if (!value)
            return null
        return {
            "photoId": String(value.photoId || ""),
            "representationId": String(value.representationId || ""),
            "locationId": String(value.locationId || ""),
            "visualHandle": String(value.visualHandle || ""),
            "decisionHeadSequence": value.decisionHeadSequence || 0,
            "decisionFlag": String(value.decisionFlag || "unflagged"),
            "decisionRating": Number(value.decisionRating || 0),
            "liked": Boolean(value.liked),
            "colorLabel": String(value.colorLabel || "none"),
            "title": String(value.title || ""),
            "sourcePath": String(value.sourcePath || ""),
            "sourceAvailable": value.sourceAvailable === undefined
                ? true : Boolean(value.sourceAvailable),
            "isRemote": Boolean(value.isRemote),
            "remoteOriginalCached": Boolean(value.remoteOriginalCached),
            "visualRole": String(value.visualRole || ""),
            "visualSource": String(value.visualSource || ""),
            "visualWidth": Number(value.visualWidth || 0),
            "visualHeight": Number(value.visualHeight || 0),
            "hasMetadata": Boolean(value.hasMetadata),
            "cameraMake": String(value.cameraMake || ""),
            "cameraModel": String(value.cameraModel || ""),
            "lensMake": String(value.lensMake || ""),
            "lensModel": String(value.lensModel || ""),
            "capturedAtUnixSeconds": value.capturedAtUnixSeconds || 0,
            "isoSpeed": Number(value.isoSpeed || 0),
            "exposureTimeSeconds": Number(value.exposureTimeSeconds || 0),
            "apertureFNumber": Number(value.apertureFNumber || 0),
            "focalLengthMm": Number(value.focalLengthMm || 0),
            "rawWidth": Number(value.rawWidth || 0),
            "rawHeight": Number(value.rawHeight || 0)
        }
    }

    function snapshotReady(snapshot) {
        return snapshot !== null
            && String(snapshot.photoId || "").length > 0
            && String(snapshot.representationId || "").length > 0
            && String(snapshot.visualSource || "").length > 0
    }

    function identityKey(snapshot) {
        if (snapshot === null)
            return ""
        return String(snapshot.photoId || "") + "\u0000"
            + String(snapshot.representationId || "")
    }

    function candidateIndex(snapshot) {
        const key = identityKey(snapshot)
        for (let index = 0; index < candidates.length; ++index) {
            if (identityKey(candidates[index]) === key)
                return index
        }
        return -1
    }

    function containsCandidate(snapshot) {
        return snapshotReady(snapshot) && candidateIndex(snapshot) >= 0
    }

    function addCandidate(value) {
        const snapshot = normalizedSnapshot(value)
        if (!snapshotReady(snapshot) || containsCandidate(snapshot))
            return false
        candidates = candidates.concat([snapshot])
        return true
    }

    function removeCandidate(value) {
        const snapshot = normalizedSnapshot(value)
        const index = candidateIndex(snapshot)
        if (index < 0)
            return false
        const next = candidates.slice()
        next.splice(index, 1)
        candidates = next
        return true
    }

    function toggleCandidate(value) {
        const snapshot = normalizedSnapshot(value)
        if (!snapshotReady(snapshot))
            return false
        return containsCandidate(snapshot)
            ? removeCandidate(snapshot) : addCandidate(snapshot)
    }

    function toggleSelectedCandidate() {
        if (typeof selection.selectedSnapshot !== "function")
            return false
        return toggleCandidate(selection.selectedSnapshot())
    }

    function clearCandidates() {
        candidates = []
        resetArena()
    }

    function cloneTiers(values) {
        const cloned = []
        for (let index = 0; index < values.length; ++index)
            cloned.push(values[index].slice())
        return cloned
    }

    function rankedCount() {
        let count = 0
        for (let index = 0; index < tiers.length; ++index)
            count += tiers[index].length
        return count
    }

    function resetArena() {
        arenaCandidates = []
        tiers = []
        unresolvedCandidates = []
        currentLeft = null
        currentRight = null
        nextCandidateIndex = 0
        searchLow = 0
        searchHigh = -1
        searchMid = -1
        comparisonCount = 0
        history = []
        arenaActive = false
        arenaComplete = false
    }

    function startArena() {
        if (!canStartArena)
            return false
        arenaCandidates = candidates.slice()
        tiers = [[arenaCandidates[0]]]
        unresolvedCandidates = []
        nextCandidateIndex = 1
        comparisonCount = 0
        history = []
        arenaActive = true
        arenaComplete = false
        beginCandidateInsertion()
        return true
    }

    function beginCandidateInsertion() {
        if (nextCandidateIndex >= arenaCandidates.length) {
            currentLeft = null
            currentRight = null
            searchMid = -1
            arenaComplete = true
            return
        }
        currentRight = arenaCandidates[nextCandidateIndex]
        searchLow = 0
        searchHigh = tiers.length - 1
        prepareComparison()
    }

    function prepareComparison() {
        if (searchLow > searchHigh) {
            const nextTiers = cloneTiers(tiers)
            nextTiers.splice(searchLow, 0, [currentRight])
            tiers = nextTiers
            nextCandidateIndex += 1
            beginCandidateInsertion()
            return
        }
        searchMid = Math.floor((searchLow + searchHigh) / 2)
        currentLeft = tiers[searchMid][0]
    }

    function pushHistory() {
        history = history.concat([{
            "tiers": cloneTiers(tiers),
            "unresolvedCandidates": unresolvedCandidates.slice(),
            "currentLeft": currentLeft,
            "currentRight": currentRight,
            "nextCandidateIndex": nextCandidateIndex,
            "searchLow": searchLow,
            "searchHigh": searchHigh,
            "searchMid": searchMid,
            "comparisonCount": comparisonCount,
            "arenaComplete": arenaComplete
        }])
    }

    function chooseLeft() {
        if (arenaComplete || currentLeft === null || currentRight === null)
            return
        pushHistory()
        comparisonCount += 1
        searchLow = searchMid + 1
        prepareComparison()
    }

    function chooseRight() {
        if (arenaComplete || currentLeft === null || currentRight === null)
            return
        pushHistory()
        comparisonCount += 1
        searchHigh = searchMid - 1
        prepareComparison()
    }

    function chooseEqual() {
        if (arenaComplete || currentLeft === null || currentRight === null)
            return
        pushHistory()
        comparisonCount += 1
        const nextTiers = cloneTiers(tiers)
        nextTiers[searchMid] = nextTiers[searchMid].concat([currentRight])
        tiers = nextTiers
        nextCandidateIndex += 1
        beginCandidateInsertion()
    }

    function skipCurrent() {
        if (arenaComplete || currentRight === null)
            return
        pushHistory()
        unresolvedCandidates = unresolvedCandidates.concat([currentRight])
        nextCandidateIndex += 1
        beginCandidateInsertion()
    }

    function undoLastChoice() {
        if (history.length === 0)
            return false
        const previous = history[history.length - 1]
        history = history.slice(0, history.length - 1)
        tiers = cloneTiers(previous.tiers)
        unresolvedCandidates = previous.unresolvedCandidates.slice()
        currentLeft = previous.currentLeft
        currentRight = previous.currentRight
        nextCandidateIndex = previous.nextCandidateIndex
        searchLow = previous.searchLow
        searchHigh = previous.searchHigh
        searchMid = previous.searchMid
        comparisonCount = previous.comparisonCount
        arenaComplete = previous.arenaComplete
        return true
    }

    function leaveArena() {
        arenaActive = false
    }

    function selectTopResult() {
        if (tiers.length === 0 || tiers[0].length === 0)
            return
        selectResult(tiers[0][0])
    }

    function selectResult(value) {
        const snapshot = normalizedSnapshot(value)
        if (!snapshotReady(snapshot))
            return
        selection.selectPhoto(snapshot, 0)
        arenaActive = false
    }
}
