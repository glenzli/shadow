pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick

// Owns click-to-zoom, trackpad pinch, and the magnifier cursor as one direct-
// manipulation lifecycle. Viewport geometry and detail transport remain owned
// by PrecisionCanvas.
Item {
    id: zoomInput

    required property bool interactionEnabled
    required property bool toolActive
    required property bool fitView
    required property real zoomFactor
    required property real fitZoomFactor

    property real pinchStartZoom: 1.0
    property bool wheelGestureActive: false

    signal zoomStepRequested(real viewportX, real viewportY, int direction)
    signal continuousZoomStarted()
    signal continuousZoomRequested(
        real viewportX, real viewportY, real zoomFactor)
    signal continuousZoomFinished()

    MouseArea {
        id: magnifierInput

        anchors.fill: parent
        enabled: zoomInput.interactionEnabled && zoomInput.toolActive
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.BlankCursor

        property bool zoomOut: false

        onPositionChanged: mouse => {
            zoomOut = (mouse.modifiers & Qt.AltModifier) !== 0
            magnifierCursor.x = mouse.x - magnifierCursor.width / 2
            magnifierCursor.y = mouse.y - magnifierCursor.height / 2
        }
        onClicked: mouse => zoomInput.zoomStepRequested(
            mouse.x,
            mouse.y,
            mouse.button === Qt.RightButton
                || (mouse.modifiers & Qt.AltModifier) !== 0 ? -1 : 1)

        Item {
            id: magnifierCursor

            width: 28
            height: 28
            visible: magnifierInput.containsMouse
            z: 2

            Rectangle {
                x: 2
                y: 2
                width: 18
                height: 18
                radius: 9
                color: Theme.previewHudStrongOverlay
                border.width: 2
                border.color: Theme.textPrimary
            }

            Rectangle {
                x: 18
                y: 18
                width: 11
                height: 3
                radius: 1.5
                rotation: 45
                transformOrigin: Item.Left
                color: Theme.textPrimary
            }

            Rectangle {
                x: 6
                y: 10
                width: 10
                height: 2
                color: Theme.textPrimary
            }

            Rectangle {
                x: 10
                y: 6
                width: 2
                height: 10
                visible: !magnifierInput.zoomOut
                color: Theme.textPrimary
            }
        }
    }

    PinchHandler {
        id: trackpadPinch

        enabled: zoomInput.interactionEnabled
        target: null
        minimumScale: 0.05
        maximumScale: 20.0

        onActiveChanged: {
            if (active) {
                zoomInput.pinchStartZoom = zoomInput.fitView
                    ? zoomInput.fitZoomFactor : zoomInput.zoomFactor
                zoomInput.continuousZoomStarted()
            } else {
                zoomInput.continuousZoomFinished()
            }
        }
        onUpdated: zoomInput.continuousZoomRequested(
            centroid.position.x,
            centroid.position.y,
            zoomInput.pinchStartZoom * scale)
    }

    WheelHandler {
        id: trackpadWheelZoom

        enabled: zoomInput.interactionEnabled
        target: null
        acceptedModifiers: Qt.ControlModifier
        blocking: true

        onWheel: event => {
            const delta = event.angleDelta.y !== 0
                ? event.angleDelta.y : event.pixelDelta.y
            if (delta === 0)
                return
            if (!zoomInput.wheelGestureActive) {
                zoomInput.wheelGestureActive = true
                zoomInput.pinchStartZoom = zoomInput.fitView
                    ? zoomInput.fitZoomFactor : zoomInput.zoomFactor
                zoomInput.continuousZoomStarted()
            }
            zoomInput.pinchStartZoom = Math.max(
                0.05, Math.min(4.0,
                    zoomInput.pinchStartZoom * Math.exp(delta / 600.0)))
            zoomInput.continuousZoomRequested(
                event.x, event.y, zoomInput.pinchStartZoom)
            wheelSettle.restart()
            event.accepted = true
        }
    }

    Timer {
        id: wheelSettle

        interval: 180
        repeat: false
        onTriggered: {
            zoomInput.wheelGestureActive = false
            zoomInput.continuousZoomFinished()
        }
    }
}
