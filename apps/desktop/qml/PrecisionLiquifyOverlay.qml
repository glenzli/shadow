pragma ComponentBehavior: Bound

import QtQuick

// Direct-manipulation owner for one ordered Liquify gesture. Push uses a local
// scene-graph mesh for zero-latency feedback. Reconstruct streams one
// provisional path through the authoritative renderer so it can recover the
// original coordinate field instead of approximating recovery as reverse push.
Item {
    id: overlay

    required property var editor
    required property var previewItem
    required property bool interactionEnabled
    required property bool previewReady
    required property string previewGeneration
    required property real outputAspectRatio

    readonly property bool commitPending:
        Boolean(previewItem.transientLiquifyPending)
    readonly property bool reconstructMode:
        Number(editor.liquifyBrushMode) === 1
        && Boolean(editor.liquifyCanReconstruct)
    property bool authoritativeCommitPending: false
    property string authoritativeBaseGeneration: ""

    // A committed local Push stays visible until an authoritative generation
    // replaces it, but it must not block the next stroke or a mode switch.
    visible: interactionEnabled || commitPending || authoritativeCommitPending
    enabled: visible

    onPreviewGenerationChanged: {
        if (authoritativeCommitPending
                && previewGeneration.length > 0
                && previewGeneration !== authoritativeBaseGeneration) {
            authoritativeCommitPending = false
            authoritativeBaseGeneration = ""
        }
    }

    onInteractionEnabledChanged: {
        if (!interactionEnabled) {
            if (input.gestureActive)
                input.finishGesture(null, true)
            else
                previewItem.cancelTransientLiquify()
        }
    }

    readonly property real brushDiameter:
        Math.max(12, 2 * Number(editor.liquifyBrushRadius)
            * Math.min(width, height))
    readonly property real hardnessDiameter: Math.max(
        4,
        brushDiameter * Math.max(0,
            Math.min(1, Number(editor.liquifyBrushHardness)))
    )

    MouseArea {
        id: input
        objectName: "liquifyStrokeInput"
        anchors.fill: parent
        visible: overlay.interactionEnabled
        // Readiness admits a gesture. Once pointer capture starts, a source
        // transition may briefly clear readiness but cannot cancel the stroke.
        enabled: visible && (gestureActive || overlay.previewReady)
        hoverEnabled: true
        preventStealing: true
        cursorShape: enabled ? Qt.BlankCursor : Qt.ArrowCursor

        property bool gestureActive: false
        property real pointerX: width / 2
        property real pointerY: height / 2
        property real lastSampleX: -1
        property real lastSampleY: -1
        property var draftPoints: []
        property bool gpuGestureActive: false
        property bool authoritativeGestureActive: false
        property int gestureKind: 0

        function normalizedPoint(x, y, pressure) {
            if (width <= 0 || height <= 0)
                return null
            const authoredPressure = Number(pressure)
            return {
                "x": Math.max(0, Math.min(1, x / width)),
                "y": Math.max(0, Math.min(1, y / height)),
                "pressure": Number.isFinite(authoredPressure)
                    && authoredPressure > 0
                    ? Math.max(0, Math.min(1, authoredPressure))
                    : 1
            }
        }

        function compactDraft() {
            const compacted = []
            for (let index = 0; index < draftPoints.length; index += 2)
                compacted.push(draftPoints[index])
            draftPoints = compacted
        }

        function appendPoint(mouse, force) {
            if (!gestureActive || mouse === null || mouse === undefined)
                return
            if (draftPoints.length >= 2048)
                compactDraft()
            const minimumSpacing = Math.max(2, overlay.brushDiameter * 0.12)
            if (!force && lastSampleX >= 0
                    && Math.hypot(mouse.x - lastSampleX,
                        mouse.y - lastSampleY) < minimumSpacing) {
                return
            }
            const normalized = normalizedPoint(
                mouse.x, mouse.y, mouse.pressure)
            if (normalized === null)
                return
            draftPoints.push(normalized)
            if (authoritativeGestureActive) {
                overlay.editor.updateLiquifyLiveStrokeFromPreview(
                    draftPoints, overlay.outputAspectRatio)
            } else if (gpuGestureActive) {
                overlay.previewItem.appendTransientLiquifyPoint(
                    normalized.x, normalized.y, normalized.pressure)
            }
            lastSampleX = mouse.x
            lastSampleY = mouse.y
        }

        function finishGesture(mouse, canceled) {
            if (!gestureActive)
                return
            if (!canceled)
                appendPoint(mouse, true)
            let committed = false
            const minimumPointCount = gestureKind === 1 ? 1 : 2
            if (authoritativeGestureActive) {
                if (!canceled && draftPoints.length >= minimumPointCount) {
                    overlay.editor.finishLiquifyLiveStroke()
                    committed = true
                    overlay.authoritativeBaseGeneration =
                        overlay.previewGeneration
                    overlay.authoritativeCommitPending = true
                } else {
                    overlay.editor.cancelLiquifyLiveStroke()
                }
            } else if (!canceled && draftPoints.length >= 2) {
                const previousStrokeCount =
                    overlay.editor.liquifyStrokes.length
                overlay.editor.addLiquifyStrokeFromPreview(
                    draftPoints, overlay.outputAspectRatio)
                committed = overlay.editor.liquifyStrokes.length
                    === previousStrokeCount + 1
            }
            if (gpuGestureActive) {
                if (canceled)
                    overlay.previewItem.cancelTransientLiquify()
                else
                    overlay.previewItem.finishTransientLiquify(committed)
            }
            gpuGestureActive = false
            authoritativeGestureActive = false
            gestureKind = 0
            gestureActive = false
            lastSampleX = -1
            lastSampleY = -1
            draftPoints = []
        }

        onPressed: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            gestureActive = true
            draftPoints = []
            lastSampleX = -1
            lastSampleY = -1
            gestureKind = overlay.reconstructMode ? 1 : 0
            if (gestureKind === 0
                    && Number(overlay.editor.liquifyBrushMode) !== 0) {
                overlay.editor.liquifyBrushMode = 0
            }
            const needsAuthoritativePath =
                gestureKind === 1 || overlay.commitPending
                || overlay.authoritativeCommitPending
            gpuGestureActive = !needsAuthoritativePath
                && overlay.previewItem.beginTransientLiquify(
                    Number(overlay.editor.liquifyBrushRadius),
                    Number(overlay.editor.liquifyBrushStrength),
                    Number(overlay.editor.liquifyBrushHardness))
            authoritativeGestureActive = !gpuGestureActive
                && overlay.editor.beginLiquifyLiveStroke()
            if (!gpuGestureActive && !authoritativeGestureActive) {
                gestureActive = false
                gestureKind = 0
                return
            }
            appendPoint(mouse, true)
        }
        onPositionChanged: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            if (pressed)
                appendPoint(mouse, false)
        }
        onReleased: mouse => finishGesture(mouse, false)
        onCanceled: finishGesture(null, true)
    }

    MouseArea {
        objectName: "liquifyPendingCursor"
        anchors.fill: parent
        z: 10
        visible: overlay.commitPending && !input.visible
        enabled: visible
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        cursorShape: Qt.BusyCursor
    }

    Item {
        id: brushCursor
        objectName: "liquifyBrushCursor"
        z: 20
        visible: input.visible && input.containsMouse
        x: input.pointerX
        y: input.pointerY
        width: 1
        height: 1

        Item {
            objectName: "liquifyBrushOuterRing"
            anchors.centerIn: parent
            width: overlay.brushDiameter
            height: width

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Theme.transparent
                border.width: 3
                border.color: Qt.rgba(0, 0, 0, 0.72)
            }

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Theme.transparent
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.94)
            }
        }

        Item {
            objectName: "liquifyBrushHardnessRing"
            anchors.centerIn: parent
            width: overlay.hardnessDiameter
            height: width
            visible: width < overlay.brushDiameter - 3

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Theme.transparent
                border.width: 3
                border.color: Qt.rgba(0, 0, 0, 0.72)
            }

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Theme.transparent
                border.width: 1
                border.color: Theme.accent
            }
        }
    }
}
