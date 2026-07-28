pragma ComponentBehavior: Bound

import QtQuick

// Paints the exact swept-disc coverage used to present one continuous retouch stroke.
// Authored sample points and centerline paths deliberately remain invisible.
Canvas {
    id: coverage

    required property var editor
    required property var points
    required property real radiusPixels
    required property color coverageColor
    property real coverageOffsetX: 0
    property real coverageOffsetY: 0

    antialiasing: true

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

    function fillCoverage(context) {
        // Composite the swept union once so overlapping authored samples do
        // not appear as darker, independent dabs.
        context.beginPath()
        let previous = null
        for (let pointIndex = 0; pointIndex < points.length; ++pointIndex) {
            const point = points[pointIndex]
            const pointX = Number(point.x) * width + coverageOffsetX
            const pointY = Number(point.y) * height + coverageOffsetY
            if (previous === null) {
                appendDisc(context, pointX, pointY, radiusPixels)
            } else {
                appendCapsule(
                    context,
                    previous.x,
                    previous.y,
                    pointX,
                    pointY,
                    radiusPixels
                )
            }
            previous = { x: pointX, y: pointY }
        }
        context.fill()
    }

    onPaint: {
        const context = getContext("2d")
        context.clearRect(0, 0, width, height)
        context.fillStyle = coverageColor
        fillCoverage(context)
    }

    Connections {
        target: coverage.editor
        function onParametersChanged() {
            coverage.requestPaint()
        }
    }

    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onVisibleChanged: {
        if (visible)
            requestPaint()
    }
    onPointsChanged: requestPaint()
    onRadiusPixelsChanged: requestPaint()
    onCoverageColorChanged: requestPaint()
    onCoverageOffsetXChanged: requestPaint()
    onCoverageOffsetYChanged: requestPaint()
    Component.onCompleted: requestPaint()
}
