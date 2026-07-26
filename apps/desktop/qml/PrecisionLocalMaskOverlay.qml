pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick

// Direct manipulation for the selected Grade Node's local mask. The backend
// remains the single owner of normalized mask values and edit history; this
// component only maps those values to the currently displayed photo surface.
Item {
    id: overlay

    required property var editor
    required property bool interactionEnabled

    readonly property var mask: editor.selectedLocalMask
    readonly property int kind: Number(mask.kind || 0)
    readonly property bool activeMask: kind >= 1 && kind <= 3
    readonly property var brushPoints: mask.brushPoints || []
    readonly property var handleRoles: kind === 1
        ? ["start", "end"]
        : kind === 2
            ? ["center", "radiusX", "radiusY", "feather"]
            : []

    visible: interactionEnabled
        && activeMask
        && editor.active
        && editor.hasSelectedGradeNode
    enabled: visible && !editor.stateBusy

    function clampNormalized(value) {
        return Math.max(0, Math.min(1, value))
    }

    function maskNumber(key, fallback) {
        const value = Number(mask[key])
        return Number.isFinite(value) ? value : fallback
    }

    function radialXDirection() {
        return maskNumber("x0", 0.5) + maskNumber("radiusX", 0.25) <= 1
            ? 1 : -1
    }

    function radialYDirection() {
        return maskNumber("y0", 0.5) + maskNumber("radiusY", 0.25) <= 1
            ? 1 : -1
    }

    function handleNormalizedX(role) {
        if (role === "start" || role === "center")
            return maskNumber("x0", 0.5)
        if (role === "end")
            return maskNumber("x1", 0.75)
        const center = maskNumber("x0", 0.5)
        const radius = maskNumber("radiusX", 0.25)
        if (role === "radiusX")
            return center + radialXDirection() * radius
        if (role === "feather") {
            return center + radialXDirection() * radius
                * (1 - maskNumber("feather", 0.35))
        }
        return center
    }

    function handleNormalizedY(role) {
        if (role === "start" || role === "center")
            return maskNumber("y0", 0.5)
        if (role === "end")
            return maskNumber("y1", 0.5)
        const center = maskNumber("y0", 0.5)
        if (role === "radiusY") {
            return center + radialYDirection()
                * maskNumber("radiusY", 0.25)
        }
        return center
    }

    function historyKey(role) {
        return "local_mask/" + role
    }

    function moveHandle(role, normalizedX, normalizedY) {
        const x = clampNormalized(normalizedX)
        const y = clampNormalized(normalizedY)
        if (role === "start" || role === "end" || role === "center") {
            editor.setSelectedLocalMaskPoint(role, x, y)
            return
        }
        if (role === "radiusX") {
            editor.setSelectedLocalMaskValue(
                "radiusX",
                Math.max(0.01, Math.abs(x - maskNumber("x0", 0.5)))
            )
            return
        }
        if (role === "radiusY") {
            editor.setSelectedLocalMaskValue(
                "radiusY",
                Math.max(0.01, Math.abs(y - maskNumber("y0", 0.5)))
            )
            return
        }
        if (role === "feather") {
            const radius = Math.max(0.01, maskNumber("radiusX", 0.25))
            const innerRatio = Math.min(
                1,
                Math.abs(x - maskNumber("x0", 0.5)) / radius
            )
            editor.setSelectedLocalMaskValue("feather", 1 - innerRatio)
        }
    }

    function appendEllipse(context, centerX, centerY, radiusX, radiusY) {
        const kappa = 0.5522847498307936
        context.moveTo(centerX + radiusX, centerY)
        context.bezierCurveTo(
            centerX + radiusX,
            centerY + kappa * radiusY,
            centerX + kappa * radiusX,
            centerY + radiusY,
            centerX,
            centerY + radiusY
        )
        context.bezierCurveTo(
            centerX - kappa * radiusX,
            centerY + radiusY,
            centerX - radiusX,
            centerY + kappa * radiusY,
            centerX - radiusX,
            centerY
        )
        context.bezierCurveTo(
            centerX - radiusX,
            centerY - kappa * radiusY,
            centerX - kappa * radiusX,
            centerY - radiusY,
            centerX,
            centerY - radiusY
        )
        context.bezierCurveTo(
            centerX + kappa * radiusX,
            centerY - radiusY,
            centerX + radiusX,
            centerY - kappa * radiusY,
            centerX + radiusX,
            centerY
        )
    }

    function fillBrushCoverage(context, radius) {
        for (let index = 0; index < brushPoints.length; ++index) {
            const point = brushPoints[index]
            const x = Number(point.x) * width
            const y = Number(point.y) * height
            context.beginPath()
            context.arc(x, y, radius, 0, Math.PI * 2)
            context.fill()
        }
    }

    Canvas {
        id: maskGuide
        anchors.fill: parent
        antialiasing: true

        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            if (!overlay.activeMask || width <= 0 || height <= 0)
                return

            const x0 = overlay.maskNumber("x0", 0.5) * width
            const y0 = overlay.maskNumber("y0", 0.5) * height
            context.lineCap = "round"
            context.lineJoin = "round"

            if (overlay.kind === 3) {
                const brushRadius = Math.max(
                    1,
                    overlay.maskNumber("radiusX", 0.035)
                        * Math.min(width, height)
                )
                // Paint coverage as an overlapping union of filled brush
                // stamps. The renderer already records dense samples, so the
                // discs form a continuous affected area without exposing the
                // raw centerline/path used to author it.
                context.fillStyle = Qt.rgba(
                    Theme.accent.r,
                    Theme.accent.g,
                    Theme.accent.b,
                    0.13
                )
                overlay.fillBrushCoverage(context, brushRadius)
                const coreRadius = brushRadius * Math.max(
                    0,
                    1 - overlay.maskNumber("feather", 0.6)
                )
                if (coreRadius >= 1) {
                    context.fillStyle = Qt.rgba(
                        Theme.accent.r,
                        Theme.accent.g,
                        Theme.accent.b,
                        0.12
                    )
                    overlay.fillBrushCoverage(context, coreRadius)
                }
                return
            }

            if (overlay.kind === 1) {
                const x1 = overlay.maskNumber("x1", 0.75) * width
                const y1 = overlay.maskNumber("y1", 0.5) * height
                context.beginPath()
                context.moveTo(x0, y0)
                context.lineTo(x1, y1)
                context.strokeStyle = "#b0000000"
                context.lineWidth = 5
                context.stroke()
                context.beginPath()
                context.moveTo(x0, y0)
                context.lineTo(x1, y1)
                context.strokeStyle = Theme.accent
                context.lineWidth = 2
                context.stroke()
                return
            }

            const radiusX = overlay.maskNumber("radiusX", 0.25) * width
            const radiusY = overlay.maskNumber("radiusY", 0.25) * height
            context.beginPath()
            overlay.appendEllipse(context, x0, y0, radiusX, radiusY)
            context.strokeStyle = "#b0000000"
            context.lineWidth = 5
            context.stroke()
            context.beginPath()
            overlay.appendEllipse(context, x0, y0, radiusX, radiusY)
            context.strokeStyle = Theme.accent
            context.lineWidth = 2
            context.stroke()

            const inner = 1 - overlay.maskNumber("feather", 0.35)
            if (inner > 0.01) {
                context.beginPath()
                overlay.appendEllipse(
                    context,
                    x0,
                    y0,
                    radiusX * inner,
                    radiusY * inner
                )
                context.globalAlpha = 0.72
                context.strokeStyle = Theme.previewCompareDivider
                context.lineWidth = 1
                context.stroke()
                context.globalAlpha = 1
            }
        }

        Connections {
            target: overlay.editor
            function onParametersChanged() {
                maskGuide.requestPaint()
            }
            function onSelectedGradeNodeChanged() {
                maskGuide.requestPaint()
            }
        }

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Component.onCompleted: requestPaint()
    }

    MouseArea {
        id: brushPointer
        anchors.fill: parent
        z: 1
        visible: overlay.kind === 3
        enabled: visible && overlay.enabled
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        cursorShape: enabled ? Qt.BlankCursor : Qt.ArrowCursor
        property real pointerX: width / 2
        property real pointerY: height / 2
        property bool gestureActive: false

        onPositionChanged: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            if (pressed) {
                overlay.editor.appendSelectedLocalMaskBrushPoint(
                    overlay.clampNormalized(mouse.x / Math.max(1, width)),
                    overlay.clampNormalized(mouse.y / Math.max(1, height)),
                    false
                )
            }
        }
        onPressed: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            gestureActive = true
            overlay.editor.beginParameterEdit("local_mask/brush")
            overlay.editor.appendSelectedLocalMaskBrushPoint(
                overlay.clampNormalized(mouse.x / Math.max(1, width)),
                overlay.clampNormalized(mouse.y / Math.max(1, height)),
                true
            )
        }
        onReleased: finishGesture()
        onCanceled: finishGesture()

        function finishGesture() {
            if (!gestureActive)
                return
            gestureActive = false
            overlay.editor.endParameterEdit("local_mask/brush")
        }
    }

    Item {
        z: 4
        visible: overlay.kind === 3
            && brushPointer.enabled
            && brushPointer.containsMouse
        x: brushPointer.pointerX
        y: brushPointer.pointerY

        Rectangle {
            anchors.centerIn: parent
            width: Math.max(
                8,
                overlay.maskNumber("radiusX", 0.035)
                    * Math.min(overlay.width, overlay.height) * 2
            )
            height: width
            radius: width / 2
            color: Theme.transparent
            border.width: 1
            border.color: Theme.previewCompareDivider

            Rectangle {
                anchors.centerIn: parent
                width: parent.width
                    * (1 - overlay.maskNumber("feather", 0.6))
                height: width
                radius: width / 2
                visible: width >= 4
                color: Theme.transparent
                border.width: 1
                border.color: Theme.accent
            }
        }
    }

    Repeater {
        model: overlay.handleRoles

        delegate: Rectangle {
            id: maskHandle

            required property string modelData
            property bool gestureActive: false

            width: modelData === "feather" ? 11 : 14
            height: width
            radius: width / 2
            x: overlay.handleNormalizedX(modelData) * overlay.width - width / 2
            y: overlay.handleNormalizedY(modelData) * overlay.height - height / 2
            color: modelData === "center" ? Theme.accent : Theme.panelRaised
            border.width: 2
            border.color: modelData === "feather"
                ? Theme.previewCompareDivider : Theme.accent
            z: 2

            MouseArea {
                id: handlePointer
                anchors.fill: parent
                anchors.margins: -7
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                preventStealing: true
                cursorShape: maskHandle.modelData === "radiusX"
                    || maskHandle.modelData === "feather"
                    ? Qt.SizeHorCursor
                    : maskHandle.modelData === "radiusY"
                        ? Qt.SizeVerCursor
                        : Qt.SizeAllCursor

                onPressed: {
                    maskHandle.gestureActive = true
                    overlay.editor.beginParameterEdit(
                        overlay.historyKey(maskHandle.modelData))
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const point = maskHandle.mapToItem(
                        overlay, mouse.x, mouse.y)
                    overlay.moveHandle(
                        maskHandle.modelData,
                        point.x / Math.max(1, overlay.width),
                        point.y / Math.max(1, overlay.height)
                    )
                }
                onReleased: overlay.finishHandleGesture(maskHandle)
                onCanceled: overlay.finishHandleGesture(maskHandle)
            }
        }
    }

    function finishHandleGesture(handle) {
        if (!handle.gestureActive)
            return
        handle.gestureActive = false
        editor.endParameterEdit(historyKey(handle.modelData))
    }
}
