pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick

// Owns one temporary culling draft and its guided duel lifecycle. The first
// pass finds only the top preference tier. Its second tier contains exactly
// the photos that lost directly to the eventual winner, which is the smallest
// evidence-backed runner-up pool that can be refined in another pass.
// The draft never mutates Library decisions; Like, rating, Pick, and album
// actions remain explicit operations after the user leaves the duel.
QtObject {
    id: culling

    required property var selection

    property var candidates: []
    property var arenaCandidates: []
    property var leaders: []
    property var runnerUps: []
    property var tiers: []
    property var unresolvedCandidates: []
    property var currentLeft: null
    property var currentRight: null
    property int nextCandidateIndex: 0
    property int refinementRound: 0
    property int comparisonCount: 0
    property var history: []
    property bool arenaActive: false
    property bool arenaComplete: false

    readonly property int candidateCount: candidates.length
    readonly property bool canStartArena: candidateCount >= 2
    readonly property bool canRefineRunnerUps:
        arenaComplete && runnerUps.length >= 2
    readonly property int rankedCandidateCount: nextCandidateIndex
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

    function refreshTiers() {
        const next = []
        if (leaders.length > 0)
            next.push(leaders.slice())
        if (runnerUps.length > 0)
            next.push(runnerUps.slice())
        tiers = next
    }

    function resetArena() {
        arenaCandidates = []
        leaders = []
        runnerUps = []
        tiers = []
        unresolvedCandidates = []
        currentLeft = null
        currentRight = null
        nextCandidateIndex = 0
        refinementRound = 0
        comparisonCount = 0
        history = []
        arenaActive = false
        arenaComplete = false
    }

    function startDuel(values, round) {
        if (values.length < 2)
            return false
        arenaCandidates = values.slice()
        leaders = [arenaCandidates[0]]
        runnerUps = []
        unresolvedCandidates = []
        nextCandidateIndex = 1
        refinementRound = round
        comparisonCount = 0
        history = []
        arenaActive = true
        arenaComplete = false
        refreshTiers()
        beginNextDuel()
        return true
    }

    function startArena() {
        return canStartArena ? startDuel(candidates, 0) : false
    }

    function refineRunnerUps() {
        return canRefineRunnerUps
            ? startDuel(runnerUps, refinementRound + 1) : false
    }

    function beginNextDuel() {
        if (nextCandidateIndex >= arenaCandidates.length) {
            currentLeft = null
            currentRight = null
            arenaComplete = true
            refreshTiers()
            return
        }
        currentLeft = leaders[0]
        currentRight = arenaCandidates[nextCandidateIndex]
    }

    function pushHistory() {
        history = history.concat([{
            "leaders": leaders.slice(),
            "runnerUps": runnerUps.slice(),
            "tiers": tiers.map(tier => tier.slice()),
            "unresolvedCandidates": unresolvedCandidates.slice(),
            "currentLeft": currentLeft,
            "currentRight": currentRight,
            "nextCandidateIndex": nextCandidateIndex,
            "comparisonCount": comparisonCount,
            "arenaComplete": arenaComplete
        }])
    }

    function finishCurrentChoice() {
        nextCandidateIndex += 1
        refreshTiers()
        beginNextDuel()
    }

    function chooseLeft() {
        if (arenaComplete || currentLeft === null || currentRight === null)
            return
        pushHistory()
        comparisonCount += 1
        runnerUps = runnerUps.concat([currentRight])
        finishCurrentChoice()
    }

    function chooseRight() {
        if (arenaComplete || currentLeft === null || currentRight === null)
            return
        pushHistory()
        comparisonCount += 1
        runnerUps = leaders.slice()
        leaders = [currentRight]
        finishCurrentChoice()
    }

    function chooseEqual() {
        if (arenaComplete || currentLeft === null || currentRight === null)
            return
        pushHistory()
        comparisonCount += 1
        leaders = leaders.concat([currentRight])
        finishCurrentChoice()
    }

    function skipCurrent() {
        if (arenaComplete || currentRight === null)
            return
        pushHistory()
        unresolvedCandidates = unresolvedCandidates.concat([currentRight])
        finishCurrentChoice()
    }

    function undoLastChoice() {
        if (history.length === 0)
            return false
        const previous = history[history.length - 1]
        history = history.slice(0, history.length - 1)
        leaders = previous.leaders.slice()
        runnerUps = previous.runnerUps.slice()
        tiers = previous.tiers.map(tier => tier.slice())
        unresolvedCandidates = previous.unresolvedCandidates.slice()
        currentLeft = previous.currentLeft
        currentRight = previous.currentRight
        nextCandidateIndex = previous.nextCandidateIndex
        comparisonCount = previous.comparisonCount
        arenaComplete = previous.arenaComplete
        return true
    }

    function leaveArena() {
        arenaActive = false
    }

    function selectTopResult() {
        if (leaders.length === 0)
            return
        selectResult(leaders[0])
    }

    function selectResult(value) {
        const snapshot = normalizedSnapshot(value)
        if (!snapshotReady(snapshot))
            return
        selection.selectPhoto(snapshot, 0)
        arenaActive = false
    }
}
