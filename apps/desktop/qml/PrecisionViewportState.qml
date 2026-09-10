pragma ComponentBehavior: Bound

import QtQuick

// Owns the view-only photo transform. Source/Recipe identity and detail rendering
// are deliberately inputs of the canvas, never consequences of pan or zoom here.
QtObject {
    id: viewport

    required property var flickable
    property real imagePixelWidth: 1
    property real imagePixelHeight: 1
    property real deviceScale: 1
    property real viewportWidth: flickable.width
    property real viewportHeight: flickable.height
    property bool fitView: true
    property real zoomFactor: 1
    property bool applyingTransform: false
    property bool continuousZoomActive: false
    property bool gestureAnchorValid: false
    property real gestureAnchorX: 0.5
    property real gestureAnchorY: 0.5

    readonly property real fitScale: Math.max(0.0001, Math.min(
        viewportWidth / Math.max(1, imagePixelWidth),
        viewportHeight / Math.max(1, imagePixelHeight)))
    readonly property real displayScale: fitView ? fitScale
        : zoomFactor / Math.max(1, deviceScale)
    readonly property real imageWidth: imagePixelWidth * displayScale
    readonly property real imageHeight: imagePixelHeight * displayScale
    // Dual comparison uses the same virtual center in both smaller panes. The
    // extra extent lets the Flickable reach the edges of either pane.
    readonly property real contentWidth: Math.max(flickable.width,
        imageWidth + flickable.width - viewportWidth)
    readonly property real contentHeight: Math.max(flickable.height,
        imageHeight + flickable.height - viewportHeight)
    readonly property real imageX: (contentWidth - imageWidth) / 2
    readonly property real imageY: (contentHeight - imageHeight) / 2
    readonly property real centerX: normalizedX(flickable.width / 2)
    readonly property real centerY: normalizedY(flickable.height / 2)

    function clamp(value, low, high) {
        return Math.max(low, Math.min(Math.max(low, high), value))
    }

    function normalizedX(x) {
        return clamp((flickable.contentX + x - imageX)
            / Math.max(0.0001, imageWidth), 0, 1)
    }

    function normalizedY(y) {
        return clamp((flickable.contentY + y - imageY)
            / Math.max(0.0001, imageHeight), 0, 1)
    }

    function place(nx, ny, x, y) {
        flickable.contentX = clamp(imageX + nx * imageWidth - x,
            0, contentWidth - flickable.width)
        flickable.contentY = clamp(imageY + ny * imageHeight - y,
            0, contentHeight - flickable.height)
    }

    function zoomAt(x, y, value) {
        if (!isFinite(value) || value <= 0)
            return
        const nx = continuousZoomActive && gestureAnchorValid
            ? gestureAnchorX : normalizedX(x)
        const ny = continuousZoomActive && gestureAnchorValid
            ? gestureAnchorY : normalizedY(y)
        if (continuousZoomActive && !gestureAnchorValid) {
            gestureAnchorX = nx
            gestureAnchorY = ny
            gestureAnchorValid = true
        }
        applyingTransform = true
        // These bindings are synchronous. Deferring placement to callLater
        // allowed the next gesture sample to read a half-applied transform.
        fitView = false
        zoomFactor = clamp(value, 0.05, 4)
        place(nx, ny, x, y)
        applyingTransform = false
    }

    function beginContinuousZoom() {
        flickable.cancelFlick()
        gestureAnchorValid = false
        continuousZoomActive = true
    }

    function finishContinuousZoom() {
        continuousZoomActive = false
        gestureAnchorValid = false
    }

    function reset() {
        applyingTransform = true
        flickable.cancelFlick()
        continuousZoomActive = false
        gestureAnchorValid = false
        fitView = true
        zoomFactor = 1
        flickable.contentX = 0
        flickable.contentY = 0
        applyingTransform = false
    }
}
