pragma ComponentBehavior: Bound

import QtQuick

// Owns the view-only detail-loupe state and coordinate contract. Camera AF is
// stored before Recipe geometry; this owner either maps the exact affine
// crop/flip/quarter-turn subset or truthfully falls back to the image center.
QtObject {
    id: state

    required property var editor
    required property var captureMetadata
    required property bool requestAvailable
    required property real deviceScale
    required property real viewportWidth
    required property real viewportHeight

    property bool shown: false
    property bool followPointer: false
    property real zoomFactor: 1.0
    property real centerX: 0.5
    property real centerY: 0.5
    property string targetKind: "center"
    property bool focusConfirmed: false
    property bool userPositioned: false

    function clamp01(value) {
        return Math.max(0, Math.min(1, value))
    }

    function mappedCameraFocus() {
        const metadata = captureMetadata || ({})
        const schema = Number(metadata.focusObservationSchemaVersion || 0)
        const source = String(metadata.focusObservationSource || "")
        let x = Number(metadata.focusObservationCenterX)
        let y = Number(metadata.focusObservationCenterY)
        if (!Boolean(metadata.hasFocusObservation) || schema !== 1
                || (source !== "camera_focus_area"
                    && source !== "camera_focus_location")
                || !isFinite(x) || !isFinite(y)
                || x < 0 || x > 1 || y < 0 || y > 1)
            return ({ "valid": false })

        const geometry = editor.photoGeometry || ({})
        const geometryEnabled = Boolean(geometry.present)
            && Boolean(geometry.enabled)
        if (geometryEnabled
                && Math.abs(Number(geometry.straightenDegrees || 0)) > 0.001)
            return ({ "valid": false })
        const liquifyStrokes = editor.liquifyStrokes || []
        if (Boolean(editor.liquifyNodeMaterialized)
                && Boolean(editor.liquifyNodeEnabled)
                && liquifyStrokes.length > 0)
            return ({ "valid": false })

        if (geometryEnabled) {
            const left = Number(geometry.cropLeft)
            const top = Number(geometry.cropTop)
            const right = Number(geometry.cropRight)
            const bottom = Number(geometry.cropBottom)
            if (!isFinite(left) || !isFinite(top) || !isFinite(right)
                    || !isFinite(bottom) || right <= left || bottom <= top
                    || x < left || x > right || y < top || y > bottom)
                return ({ "valid": false })
            x = (x - left) / (right - left)
            y = (y - top) / (bottom - top)
            if (Boolean(geometry.flipHorizontal))
                x = 1 - x
            if (Boolean(geometry.flipVertical))
                y = 1 - y
            const quarterTurn = ((Number(geometry.quarterTurn || 0) % 4) + 4) % 4
            const sourceX = x
            const sourceY = y
            if (quarterTurn === 1) {
                x = 1 - sourceY
                y = sourceX
            } else if (quarterTurn === 2) {
                x = 1 - sourceX
                y = 1 - sourceY
            } else if (quarterTurn === 3) {
                x = sourceY
                y = 1 - sourceX
            }
        }
        return {
            "valid": true,
            "x": clamp01(x),
            "y": clamp01(y),
            "confirmed": Boolean(metadata.focusObservationConfirmed)
        }
    }

    function resetTargetFromMetadata() {
        const focus = mappedCameraFocus()
        centerX = focus.valid ? focus.x : 0.5
        centerY = focus.valid ? focus.y : 0.5
        targetKind = focus.valid ? "camera" : "center"
        focusConfirmed = focus.valid && focus.confirmed
        userPositioned = false
    }

    function requestDetail() {
        if (!shown || !requestAvailable) {
            editor.leaveDetailMode()
            return
        }
        const pixelWidth = Math.max(64, Math.ceil(
            viewportWidth * deviceScale / zoomFactor))
        const pixelHeight = Math.max(64, Math.ceil(
            viewportHeight * deviceScale / zoomFactor))
        editor.requestDetailViewport(centerX, centerY, pixelWidth, pixelHeight)
    }

    function open() {
        if (!requestAvailable)
            return
        followPointer = false
        zoomFactor = 1.0
        resetTargetFromMetadata()
        shown = true
        Qt.callLater(requestDetail)
    }

    function close() {
        requestSettle.stop()
        shown = false
        followPointer = false
        editor.leaveDetailMode()
    }

    function setTarget(normalizedX, normalizedY) {
        centerX = clamp01(normalizedX)
        centerY = clamp01(normalizedY)
        targetKind = "manual"
        focusConfirmed = false
        userPositioned = true
        requestSettle.restart()
    }

    function toggleFollowPointer() {
        followPointer = !followPointer
    }

    function setZoomFactor(value) {
        zoomFactor = value
        requestDetail()
    }

    onCaptureMetadataChanged: {
        if (shown && !userPositioned) {
            resetTargetFromMetadata()
            requestSettle.restart()
        }
    }

    onDeviceScaleChanged: {
        if (shown) {
            editor.leaveDetailMode()
            Qt.callLater(requestDetail)
        }
    }

    property Timer requestSettle: Timer {
        interval: 80
        repeat: false
        onTriggered: state.requestDetail()
    }

    property Connections editorConnections: Connections {
        target: state.editor

        function onParametersChanged() {
            if (state.shown && !state.userPositioned) {
                state.resetTargetFromMetadata()
                state.requestSettle.restart()
            }
        }

        function onSourcePathChanged() {
            state.close()
        }
    }
}
