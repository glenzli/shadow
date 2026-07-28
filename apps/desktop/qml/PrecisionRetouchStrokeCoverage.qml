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

    function fillDisc(context, x, y, radius) {
        context.beginPath()
        context.arc(x, y, radius, 0, Math.PI * 2)
        context.fill()
    }

    function fillCapsule(context, x0, y0, x1, y1, radius) {
        const deltaX = x1 - x0
        const deltaY = y1 - y0
        const length = Math.hypot(deltaX, deltaY)
        if (length < 0.01) {
            fillDisc(context, x0, y0, radius)
            return
        }
        const normalX = -deltaY / length * radius
        const normalY = deltaX / length * radius
        context.beginPath()
        context.moveTo(x0 + normalX, y0 + normalY)
        context.lineTo(x1 + normalX, y1 + normalY)
        context.lineTo(x1 - normalX, y1 - normalY)
        context.lineTo(x0 - normalX, y0 - normalY)
        context.closePath()
        context.fill()
        fillDisc(context, x0, y0, radius)
        fillDisc(context, x1, y1, radius)
    }

    function fillCoverage(context) {
        let previous = null
        for (let pointIndex = 0; pointIndex < points.length; ++pointIndex) {
            const point = points[pointIndex]
            const pointX = Number(point.x) * width + coverageOffsetX
            const pointY = Number(point.y) * height + coverageOffsetY
            if (previous === null) {
                fillDisc(context, pointX, pointY, radiusPixels)
            } else {
                fillCapsule(
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
