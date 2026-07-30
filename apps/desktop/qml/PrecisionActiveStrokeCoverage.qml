pragma ComponentBehavior: Bound

import QtQuick

// Incremental presentation for one pointer gesture that has not yet entered
// the edit recipe. It paints only newly swept capsules; persistent mask and
// retouch coverage remain owned by their feature-specific overlays.
Canvas {
    id: coverage

    required property real radiusPixels
    required property color coverageColor
    property var pendingSegments: []
    property bool clearRequested: true

    antialiasing: true
    opacity: coverageColor.a

    function appendDisc(context, x, y, radius) {
        context.moveTo(x + radius, y)
        context.arc(x, y, radius, 0, Math.PI * 2)
    }

    function appendCapsule(context, x0, y0, x1, y1, radius) {
        const deltaX = x1 - x0
        const deltaY = y1 - y0
        const length = Math.hypot(deltaX, deltaY)
        if (length < 0.01) {
            appendDisc(context, x0, y0, radius)
            return
        }
        const normalX = -deltaY / length * radius
        const normalY = deltaX / length * radius
        context.moveTo(x0 + normalX, y0 + normalY)
        context.lineTo(x1 + normalX, y1 + normalY)
        context.lineTo(x1 - normalX, y1 - normalY)
        context.lineTo(x0 - normalX, y0 - normalY)
        context.closePath()
        appendDisc(context, x0, y0, radius)
        appendDisc(context, x1, y1, radius)
    }

    function beginStroke(x, y) {
        clearRequested = true
        pendingSegments = [{
            "x0": x, "y0": y, "x1": x, "y1": y
        }]
        requestPaint()
    }

    function appendSegment(x0, y0, x1, y1) {
        pendingSegments.push({
            "x0": x0, "y0": y0, "x1": x1, "y1": y1
        })
        requestPaint()
    }

    function clearStroke() {
        clearRequested = true
        pendingSegments = []
        requestPaint()
    }

    onPaint: {
        const context = getContext("2d")
        if (clearRequested) {
            context.clearRect(0, 0, width, height)
            clearRequested = false
        }
        const segments = pendingSegments
        pendingSegments = []
        if (segments.length === 0 || radiusPixels <= 0)
            return

        context.fillStyle = Qt.rgba(
            coverageColor.r,
            coverageColor.g,
            coverageColor.b,
            1
        )
        context.beginPath()
        for (let index = 0; index < segments.length; ++index) {
            const segment = segments[index]
            appendCapsule(
                context,
                Number(segment.x0),
                Number(segment.y0),
                Number(segment.x1),
                Number(segment.y1),
                radiusPixels
            )
        }
        context.fill()
    }

    onWidthChanged: clearStroke()
    onHeightChanged: clearStroke()
}
