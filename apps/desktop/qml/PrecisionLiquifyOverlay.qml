pragma ComponentBehavior: Bound

import QtQuick

// Direct-manipulation owner for one Liquify push gesture. Pointer samples and
// swept coverage remain transient here; only release crosses into the
// controller, producing one durable stroke and one history transition.
Item {
    id: overlay

    required property var editor
    required property var previewItem
    required property bool interactionEnabled
    required property real outputAspectRatio

    readonly property bool commitPending:
        Boolean(previewItem.transientLiquifyPending)

    // The pending state outlives the authoring MouseArea until the next
    // authoritative preview generation replaces the transient mesh.
    visible: interactionEnabled || commitPending
    enabled: visible

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

    PrecisionActiveStrokeCoverage {
        id: activeCoverage
        anchors.fill: parent
        visible: input.gestureActive
        radiusPixels: overlay.brushDiameter / 2
        coverageColor: Theme.maskCoverageTint
    }

    MouseArea {
        id: input
        objectName: "liquifyStrokeInput"
        anchors.fill: parent
        visible: overlay.interactionEnabled && !overlay.commitPending
        enabled: visible
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
            if (lastSampleX >= 0) {
                activeCoverage.appendSegment(
                    lastSampleX, lastSampleY, mouse.x, mouse.y)
            }
            draftPoints.push(normalized)
            if (gpuGestureActive) {
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
            if (!canceled && draftPoints.length >= 2) {
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
            gestureActive = false
            lastSampleX = -1
            lastSampleY = -1
            draftPoints = []
            Qt.callLater(activeCoverage.clearStroke)
        }

        onPressed: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            gestureActive = true
            draftPoints = []
            lastSampleX = -1
            lastSampleY = -1
            gpuGestureActive =
                overlay.previewItem.beginTransientLiquify(
                    Number(overlay.editor.liquifyBrushRadius),
                    Number(overlay.editor.liquifyBrushStrength),
                    Number(overlay.editor.liquifyBrushHardness))
            activeCoverage.beginStroke(mouse.x, mouse.y)
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
        visible: overlay.commitPending
        enabled: visible
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        cursorShape: Qt.BusyCursor
    }

    Item {
        visible: input.visible && input.containsMouse
        x: input.pointerX
        y: input.pointerY
        width: 1
        height: 1

        Rectangle {
            anchors.centerIn: parent
            width: overlay.brushDiameter
            height: width
            radius: width / 2
            color: Theme.transparent
            border.width: 1
            border.color: Theme.previewCompareDivider

            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                radius: width / 2
                color: Theme.transparent
                border.width: 1
                border.color: Theme.accent
            }
        }
    }
}
