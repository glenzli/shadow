pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick

// Transient authoring only. The controller owns normalized brush points and
// candidate identity; accepted pixels enter the photo Recipe through the fixed
// AI Completion node.
Item {
    id: overlay

    required property var editor
    property bool interactionEnabled: false

    function requestRepaint() {
        selectionCanvas.requestPaint()
    }

    Image {
        anchors.fill: parent
        z: 1
        source: overlay.editor.imageCompletionCandidateSource
        visible: overlay.editor.imageCompletionHasCandidate
        fillMode: Image.Stretch
        asynchronous: false
        cache: false
        smooth: true
    }

    Canvas {
        id: selectionCanvas
        visible: !overlay.editor.imageCompletionHasCandidate
        anchors.fill: parent
        z: 2
        renderTarget: Canvas.FramebufferObject

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onVisibleChanged: if (visible) requestPaint()
        onPaint: {
            const context = getContext("2d")
            context.reset()
            context.clearRect(0, 0, width, height)
            const points = overlay.editor.imageCompletionBrushPoints
            let previous = null
            for (let index = 0; index < points.length; ++index) {
                const point = points[index]
                const x = Number(point.x) * width
                const y = Number(point.y) * height
                const radius = Math.max(1.5,
                    Number(point.radius) * Math.min(width, height))
                const erase = Boolean(point.erase)
                context.globalCompositeOperation = erase
                    ? "destination-out" : "source-over"
                context.fillStyle = "rgba(235, 78, 113, 0.45)"
                context.strokeStyle = "rgba(235, 78, 113, 0.45)"
                context.lineCap = "round"
                context.lineJoin = "round"
                context.lineWidth = radius * 2
                if (previous !== null
                        && Number(previous.strokeId) === Number(point.strokeId)
                        && Boolean(previous.erase) === erase) {
                    context.beginPath()
                    context.moveTo(Number(previous.x) * width,
                        Number(previous.y) * height)
                    context.lineTo(x, y)
                    context.stroke()
                } else {
                    context.beginPath()
                    context.arc(x, y, radius, 0, Math.PI * 2)
                    context.fill()
                }
                previous = point
            }
            context.globalCompositeOperation = "source-over"
        }
    }

    MouseArea {
        id: brushInput
        anchors.fill: parent
        z: 3
        enabled: overlay.interactionEnabled
            && overlay.editor.imageCompletionActive
            && !overlay.editor.imageCompletionBusy
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.CrossCursor
        property int strokeId: 0

        function append(mouse) {
            if (strokeId === 0 || width <= 0 || height <= 0)
                return
            overlay.editor.addImageCompletionBrushPoint(
                Math.max(0, Math.min(1, mouse.x / width)),
                Math.max(0, Math.min(1, mouse.y / height)),
                strokeId)
        }

        onPressed: mouse => {
            strokeId = overlay.editor.beginImageCompletionStroke()
            append(mouse)
        }
        onPositionChanged: mouse => {
            if (pressed)
                append(mouse)
        }
        onReleased: strokeId = 0
        onCanceled: strokeId = 0
    }

    Connections {
        target: overlay.editor
        function onImageCompletionChanged() {
            overlay.requestRepaint()
        }
    }
}
