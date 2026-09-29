pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Direct-manipulation owner for preview sampling and repair-stroke authoring.
// The canvas supplies one generation-verified preview surface and its painted
// content rectangle; this component owns coordinate normalization, picker
// routing, transient stroke sampling, terminal commit, and the pointer
// affordance as one interaction lifecycle.
Item {
    id: pickerInput

    required property var editor
    required property Item previewItem
    property Item samplePreviewItem: null
    required property rect previewContentRect
    required property bool previewFrameReady
    required property string readyPreviewGeneration
    required property real displayScale
    required property real levelZeroWidth
    required property real levelZeroHeight
    required property bool interactionEnabled
    // The sampled anchor belongs to authoring the next repair. Once an
    // authored region is selected, its own exact donor outline is the single
    // source authority shown on canvas; keeping this session marker visible
    // would present two unrelated-looking source positions.
    property int selectedRetouchIndex: -1

    visible: interactionEnabled
    enabled: visible

    function showRetouchBrushSizeHud() {
        retouchBrushSizeHudTimer.restart()
    }

    Shortcut {
        objectName: "retouchBrushSmallerShortcut"
        sequence: "["
        context: Qt.ApplicationShortcut
        enabled: pickerInput.enabled && pickerInput.editor.retouchPickerActive
            && !pickerInput.editor.stateBusy
        onActivated: {
            pickerInput.editor.adjustRetouchBrushRadius(-1)
            pickerInput.showRetouchBrushSizeHud()
        }
    }

    Shortcut {
        objectName: "retouchBrushLargerShortcut"
        sequence: "]"
        context: Qt.ApplicationShortcut
        enabled: pickerInput.enabled && pickerInput.editor.retouchPickerActive
            && !pickerInput.editor.stateBusy
        onActivated: {
            pickerInput.editor.adjustRetouchBrushRadius(1)
            pickerInput.showRetouchBrushSizeHud()
        }
    }

    Timer {
        id: retouchBrushSizeHudTimer
        interval: 850
    }

    function normalizedContentPoint(sourceItem, sourceX, sourceY) {
        if (previewContentRect.width <= 0 || previewContentRect.height <= 0) {
            return null
        }
        const mapped = previewItem.mapFromItem(sourceItem, sourceX, sourceY)
        const paintedWidth = Math.max(1, previewContentRect.width)
        const paintedHeight = Math.max(1, previewContentRect.height)
        const paintedX = previewContentRect.x
        const paintedY = previewContentRect.y
        if (mapped.x < paintedX || mapped.y < paintedY
                || mapped.x > paintedX + paintedWidth
                || mapped.y > paintedY + paintedHeight) {
            return null
        }
        return Qt.point(
            Math.max(0, Math.min(1, (mapped.x - paintedX) / paintedWidth)),
            Math.max(0, Math.min(1, (mapped.y - paintedY) / paintedHeight))
        )
    }

    function pickPreviewColor(sourceItem, sourceX, sourceY) {
        if (!previewFrameReady || readyPreviewGeneration.length === 0)
            return
        const normalized = normalizedContentPoint(
            sourceItem, sourceX, sourceY)
        if (normalized === null)
            return
        if (editor.rawWhiteBalancePickerActive) {
            editor.setFoundationWhiteBalanceFromSource(
                normalized.x, normalized.y)
        } else if (editor.whiteBalancePickerActive) {
            editor.setWhiteBalanceFromPreview(
                normalized.x, normalized.y, readyPreviewGeneration)
        } else {
            editor.addPointColorFromPreview(
                normalized.x, normalized.y, readyPreviewGeneration)
        }
    }

    function sampledSourceCanvasPoint() {
        const source = editor.retouchSampledSource
        if (!editor.retouchSourceSampled || source === undefined
                || !Number.isFinite(Number(source.x))
                || !Number.isFinite(Number(source.y))) {
            return Qt.point(-1000, -1000)
        }
        return previewItem.mapToItem(
            pickerInput,
            previewContentRect.x + Number(source.x) * previewContentRect.width,
            previewContentRect.y + Number(source.y) * previewContentRect.height
        )
    }

    PrecisionActiveStrokeCoverage {
        id: activeRetouchCoverage
        objectName: "activeRetouchCoverage"
        anchors.fill: parent
        z: 1
        visible: inputArea.retouchStrokeActive
        radiusPixels: inputArea.retouchBrushDiameter() / 2
        coverageColor: Theme.maskCoverageTint
    }

    PrecisionRetouchSourcePreview {
        id: sourcePreview
        readonly property var anchor: {
            if (pickerInput.samplePreviewItem === null || !pickerInput.editor.retouchSourceSampled) return ({});
            // Depend on source changes as well as pointer movement.
            const sampled = pickerInput.editor.retouchSampledSource;
            return pickerInput.editor.retouchSources.previewSourceAt(
                inputArea.pointerX / Math.max(1, pickerInput.width),
                inputArea.pointerY / Math.max(1, pickerInput.height));
        }
        sampleItem: pickerInput.samplePreviewItem
        sourcePoint: Qt.point(Number(anchor.x || 0), Number(anchor.y || 0))
        width: inputArea.retouchBrushDiameter()
        height: width
        x: inputArea.pointerX - width / 2
        y: inputArea.pointerY - height / 2
        z: 1
        amount: pickerInput.editor.retouchSources.previewOpacity
        visible: pickerInput.samplePreviewItem !== null && pickerInput.previewFrameReady
            && pickerInput.editor.retouchPickerActive && !pickerInput.editor.retouchSourcePicking
            && pickerInput.editor.retouchSourceSampled && pickerInput.editor.retouchSources.previewEnabled
            && inputArea.containsMouse && !inputArea.retouchStrokeActive
            && Number(anchor.x) >= 0 && Number(anchor.x) <= 1 && Number(anchor.y) >= 0 && Number(anchor.y) <= 1
    }

    MouseArea {
        id: inputArea
        objectName: "retouchStrokeInputArea"
        anchors.fill: parent
        z: 2
        hoverEnabled: true
        cursorShape: enabled ? Qt.BlankCursor : Qt.ArrowCursor
        preventStealing: true

        property real pointerX: width / 2
        property real pointerY: height / 2
        property bool retouchGestureActive: false
        property bool retouchStrokeActive: false
        property real retouchPressX: -1
        property real retouchPressY: -1
        property real lastRetouchStrokeX: -1
        property real lastRetouchStrokeY: -1
        property var retouchPressPoint: null
        property var retouchDraftPoints: []
        property var retouchRawPoints: []
        property bool strokeLimitReached: false

        // Iterative RDP in source pixels. Every retained segment is checked against
        // the original samples, so repeated compaction cannot accumulate error.
        function compactPath(points) {
            if (points.length <= 2) return points.slice()
            const tolerance = Math.max(0.1, Number(pickerInput.editor.retouchBrushRadius) * 0.08)
            const keep = new Set([0, points.length - 1])
            const pending = [[0, points.length - 1]]
            while (pending.length > 0) {
                const range = pending.pop()
                const a = points[range[0]], b = points[range[1]]
                const dx = (b.x - a.x) * pickerInput.levelZeroWidth
                const dy = (b.y - a.y) * pickerInput.levelZeroHeight
                const length2 = dx * dx + dy * dy
                let farthest = -1, distance2 = tolerance * tolerance
                for (let i = range[0] + 1; i < range[1]; ++i) {
                    const x = (points[i].x - a.x) * pickerInput.levelZeroWidth
                    const y = (points[i].y - a.y) * pickerInput.levelZeroHeight
                    const t = length2 > 0 ? Math.max(0, Math.min(1, (x * dx + y * dy) / length2)) : 0
                    const ex = x - t * dx, ey = y - t * dy
                    const d = ex * ex + ey * ey
                    if (d > distance2) { distance2 = d; farthest = i }
                }
                if (farthest >= 0) {
                    keep.add(farthest)
                    if (keep.size > 512) return points
                    pending.push([range[0], farthest], [farthest, range[1]])
                }
            }
            const result = []
            for (let i = 0; i < points.length; ++i)
                if (keep.has(i)) result.push(points[i])
            return result
        }

        function redrawDraft() {
            activeRetouchCoverage.clearStroke()
            let previous = null
            for (const p of retouchDraftPoints) {
                const position = pickerInput.previewItem.mapToItem(inputArea,
                    pickerInput.previewContentRect.x + p.x * pickerInput.previewContentRect.width,
                    pickerInput.previewContentRect.y + p.y * pickerInput.previewContentRect.height)
                if (previous === null) activeRetouchCoverage.beginStroke(position.x, position.y)
                else activeRetouchCoverage.appendSegment(previous.x, previous.y, position.x, position.y)
                previous = position
            }
        }

        function retouchBrushDiameter() {
            if (pickerInput.editor.retouchSources !== undefined
                    && pickerInput.editor.retouchSources.projectionRequired === true) {
                const p = pickerInput.normalizedContentPoint(inputArea, pointerX, pointerY)
                if (p !== null) {
                    const radius = pickerInput.editor.retouchSources.previewRadius(p.x, p.y)
                    if (radius > 0) return Math.max(0.5, 2 * radius * pickerInput.previewContentRect.width)
                }
            }
            // Coverage follows the level-zero radius that will be authored on
            // release. Keep it exact even when the image is fitted below 1:1.
            return Math.max(
                0.5,
                2 * Number(pickerInput.editor.retouchBrushRadius)
                    * pickerInput.displayScale
            )
        }

        function appendRetouchDraftPoint(mouse, force) {
            if (strokeLimitReached) return
            const normalized = pickerInput.normalizedContentPoint(
                inputArea, mouse.x, mouse.y)
            if (normalized === null)
                return
            // Keep a compact sampled path while leaving the renderer
            // responsible for joining every adjacent pair into one swept
            // brush region.
            const minimumSpacing = Math.max(
                2, retouchBrushDiameter() * 0.18)
            if (!force
                    && Math.hypot(
                        mouse.x - lastRetouchStrokeX,
                        mouse.y - lastRetouchStrokeY
                    ) < minimumSpacing) {
                return
            }
            const raw = retouchRawPoints.concat([{ "x": normalized.x, "y": normalized.y }])
            if (raw.length > 8192) { strokeLimitReached = true; return }
            const compacted = raw.length > 512 ? compactPath(raw) : raw
            if (compacted.length > 512) { strokeLimitReached = true; return }
            retouchRawPoints = raw
            retouchDraftPoints = compacted
            if (raw.length > 512) {
                redrawDraft()
            } else if (lastRetouchStrokeX >= 0) {
                activeRetouchCoverage.appendSegment(lastRetouchStrokeX, lastRetouchStrokeY, mouse.x, mouse.y)
            }
            lastRetouchStrokeX = mouse.x
            lastRetouchStrokeY = mouse.y
        }

        function finishRetouchGesture(mouse, canceled) {
            if (!retouchGestureActive)
                return
            if (retouchStrokeActive && !canceled) {
                if (mouse !== undefined && mouse !== null)
                    appendRetouchDraftPoint(mouse, true)
                if (retouchDraftPoints.length > 0) {
                    pickerInput.editor.addRetouchStrokeFromPreview(
                        retouchDraftPoints,
                        pickerInput.readyPreviewGeneration,
                        Math.max(1, Math.round(pickerInput.levelZeroWidth)),
                        Math.max(1, Math.round(pickerInput.levelZeroHeight)))
                }
                Qt.callLater(activeRetouchCoverage.clearStroke)
            } else if (!canceled && retouchPressPoint !== null) {
                // A click is a one-point continuous region. Legacy spot
                // records remain editable, but all newly authored repairs
                // now share one ordered stroke collection.
                pickerInput.editor.addRetouchStrokeFromPreview(
                    [{
                        "x": retouchPressPoint.x,
                        "y": retouchPressPoint.y
                    }],
                    pickerInput.readyPreviewGeneration,
                    Math.max(1, Math.round(pickerInput.levelZeroWidth)),
                    Math.max(1, Math.round(pickerInput.levelZeroHeight)))
            }
            activeRetouchCoverage.clearStroke()
            retouchRawPoints = []
            strokeLimitReached = false
            retouchGestureActive = false
            retouchStrokeActive = false
            retouchPressPoint = null
            retouchPressX = -1
            retouchPressY = -1
            lastRetouchStrokeX = -1
            lastRetouchStrokeY = -1
            retouchDraftPoints = []
        }

        onPositionChanged: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            if (!pressed || !retouchGestureActive)
                return
            if (!retouchStrokeActive) {
                const dragThreshold = Math.max(
                    3, retouchBrushDiameter() * 0.12)
                if (Math.hypot(
                        mouse.x - retouchPressX,
                        mouse.y - retouchPressY
                    ) < dragThreshold) {
                    return
                }
                retouchStrokeActive = true
                lastRetouchStrokeX = retouchPressX
                lastRetouchStrokeY = retouchPressY
                retouchDraftPoints = [{
                    "x": retouchPressPoint.x,
                    "y": retouchPressPoint.y
                }]
                retouchRawPoints = retouchDraftPoints.slice()
                activeRetouchCoverage.beginStroke(
                    retouchPressX, retouchPressY)
            }
            appendRetouchDraftPoint(mouse, false)
        }

        onPressed: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            if (!pickerInput.editor.retouchPickerActive)
                return
            const normalized = pickerInput.normalizedContentPoint(
                inputArea, mouse.x, mouse.y)
            if (normalized === null)
                return
            // Explicit source-pick mode makes Clone a visible two-step
            // operation. Option/Alt remains the fast way to replace the
            // source while painting either repair mode.
            if (pickerInput.editor.retouchSourcePicking
                    || (mouse.modifiers & Qt.AltModifier) !== 0) {
                pickerInput.editor.setRetouchSourceFromPreview(
                    normalized.x, normalized.y)
                retouchGestureActive = false
                mouse.accepted = true
                return
            }
            retouchGestureActive = true
            retouchStrokeActive = false
            retouchPressX = mouse.x
            retouchPressY = mouse.y
            retouchPressPoint = normalized
            retouchDraftPoints = []
        }
        onReleased: mouse => finishRetouchGesture(mouse, false)
        onCanceled: finishRetouchGesture(null, true)
        onEnabledChanged: { if (!enabled) finishRetouchGesture(null, true) }
        onVisibleChanged: { if (!visible) finishRetouchGesture(null, true) }
        onClicked: mouse => {
            if (!pickerInput.editor.retouchPickerActive) {
                pickerInput.pickPreviewColor(
                    inputArea, mouse.x, mouse.y)
            }
        }
    }

    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        visible: inputArea.strokeLimitReached
        text: qsTr("Stroke limit reached. Release, then continue with a new stroke.")
        color: Theme.warningText
        padding: 8
        background: Rectangle { color: Theme.panel; radius: 6 }
    }

    // Sampling has to leave an immediately manipulable object on the canvas:
    // it is a session-only anchor, not an authored repair region. This keeps
    // the familiar sample → paint workflow visible before the first stroke.
    Item {
        id: sampledSourceMarker
        objectName: "retouchSampledSourceMarker"

        readonly property point canvasPoint: pickerInput.sampledSourceCanvasPoint()

        z: 4
        visible: pickerInput.editor.retouchPickerActive
            && pickerInput.editor.retouchSourceSampled
            && (pickerInput.editor.retouchSourcePicking
                || pickerInput.selectedRetouchIndex < 0)
        width: 34
        height: 34
        x: canvasPoint.x - width / 2
        y: canvasPoint.y - height / 2

        Rectangle {
            anchors.centerIn: parent
            width: 18
            height: 18
            radius: width / 2
            color: Qt.rgba(
                Theme.accent.r,
                Theme.accent.g,
                Theme.accent.b,
                0.18
            )
            border.width: 1.5
            border.color: Theme.accent
        }

        Rectangle {
            anchors.centerIn: parent
            width: 1
            height: 30
            color: Theme.accent
        }

        Rectangle {
            anchors.centerIn: parent
            width: 30
            height: 1
            color: Theme.accent
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.top
            anchors.bottomMargin: 3
            text: qsTr("SOURCE")
            color: Theme.accent
            font.pixelSize: Theme.fontCaption
            font.bold: true
        }

        MouseArea {
            objectName: "retouchSampledSourceMarkerHitArea"

            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.CrossCursor

            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const point = sampledSourceMarker.mapToItem(
                    inputArea, mouse.x, mouse.y)
                const normalized = pickerInput.normalizedContentPoint(
                    inputArea, point.x, point.y)
                if (normalized !== null) {
                    pickerInput.editor.moveRetouchSourceFromPreview(
                        normalized.x, normalized.y)
                }
            }
        }
    }

    Item {
        z: 3
        visible: inputArea.containsMouse
        x: inputArea.pointerX
        y: inputArea.pointerY

        Rectangle {
            objectName: "retouchBrushCursor"
            visible: pickerInput.editor.retouchPickerActive
            anchors.centerIn: parent
            width: inputArea.retouchBrushDiameter()
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

            Rectangle {
                anchors.centerIn: parent
                width: 4
                height: 4
                radius: 2
                color: Theme.previewCompareDivider
            }

            Label {
                visible: pickerInput.editor.retouchSourcePicking
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.top
                anchors.bottomMargin: 5
                text: qsTr("SOURCE")
                color: Theme.accent
                font.pixelSize: Theme.fontCaption
                font.bold: true

                Rectangle {
                    z: -1
                    anchors.fill: parent
                    anchors.margins: -4
                    radius: 3
                    color: Theme.previewHudStrongOverlay
                }
            }

            Label {
                visible: retouchBrushSizeHudTimer.running
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.bottom
                anchors.topMargin: 6
                text: qsTr("%1 px").arg(pickerInput.editor.retouchBrushRadius)
                color: Theme.previewCompareDivider
                font.pixelSize: Theme.fontMeta
                font.bold: true

                Rectangle {
                    z: -1
                    anchors.fill: parent
                    anchors.margins: -4
                    radius: 3
                    color: Theme.previewHudStrongOverlay
                }
            }
        }

        Item {
            visible: !pickerInput.editor.retouchPickerActive
            x: -6
            y: -19
            width: 24
            height: 24

            ShadowIcon {
                x: 1
                y: 1
                source: "qrc:/icons/eyedropper.svg"
                color: Theme.previewHudStrongOverlay
                size: 24
            }
            ShadowIcon {
                source: "qrc:/icons/eyedropper.svg"
                color: Theme.previewCompareDivider
                size: 24
            }
        }
    }
}
