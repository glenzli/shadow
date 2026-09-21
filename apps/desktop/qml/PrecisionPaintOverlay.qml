pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: overlay
    required property var editor
    required property bool interactionEnabled
    required property bool previewReady
    required property string previewGeneration
    required property real outputAspectRatio
    readonly property var paint: editor.paint
    property bool restoreErase: false
    property bool tabletErase: false
    onOutputAspectRatioChanged: cursor.updateTip()
    visible: interactionEnabled
    onInteractionEnabledChanged: {
        if (!interactionEnabled) { input.cancel(); paint.cancelStroke(); paint.picking = false }
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_BracketLeft) { paint.radius = Math.max(0.0001, paint.radius / 1.2); event.accepted = true }
        else if (event.key === Qt.Key_BracketRight) { paint.radius = Math.min(0.1, paint.radius * 1.2); event.accepted = true }
        else if (event.key === Qt.Key_X && !paint.strokeActive) { if (paint.dodgeBurn) { paint.erase = false; paint.burn = !paint.burn } else paint.brushSlot = 1 - paint.brushSlot; event.accepted = true }
        else if (event.key === Qt.Key_Escape && paint.strokeActive) { input.cancel(); event.accepted = true }
    }
    function restoreTabletMode() {
        if (tabletErase) { paint.erase = restoreErase; tabletErase = false }
    }
    PaintStrokeInput {
        id: input
        objectName: "paintStrokeInput"
        anchors.fill: parent
        enabled: overlay.interactionEnabled && (overlay.paint.strokeActive || (overlay.previewReady && overlay.paint.canPaint))
        pointerCursor: overlay.paint.picking ? Qt.CrossCursor : Qt.BlankCursor
        onStrokePressed: (x, y, pressure, modifiers, eraser) => {
            overlay.forceActiveFocus()
            if (overlay.paint.picking || (!overlay.paint.dodgeBurn && (modifiers & Qt.AltModifier))) {
                overlay.paint.sampleColor(x / width, y / height, overlay.previewGeneration)
                return
            }
            if (eraser) {
                overlay.restoreErase = overlay.paint.erase
                overlay.tabletErase = true
                overlay.paint.erase = true
            }
            overlay.paint.beginStroke(x / width, y / height, overlay.outputAspectRatio, pressure, Boolean(overlay.paint.dodgeBurn && (modifiers & Qt.AltModifier)))
        }
        onStrokeMoved: (x, y, pressure) => {
            if (overlay.paint.strokeActive) overlay.paint.appendPoint(x / width, y / height, pressure)
        }
        onStrokeReleased: {
            overlay.paint.finishStroke()
            overlay.restoreTabletMode()
        }
        onStrokeCanceled: {
            overlay.paint.cancelStroke()
            overlay.restoreTabletMode()
        }
        onPointerChanged: cursor.updateTip()
    }
    Connections {
        target: overlay.paint
        function onBrushChanged() { cursor.updateTip() }
    }
    Connections {
        target: overlay.editor
        function onParametersChanged() { if (cursor.visible) cursor.updateTip() }
    }
    Canvas {
        id: cursor
        property var tip: ({})
        readonly property real pressureScale: overlay.paint.strokeActive && overlay.paint.pressureSize ? 0.1 + 0.9 * input.pressure : 1
        function updateTip() {
            if (overlay.width > 0 && overlay.height > 0)
                tip = overlay.paint.cursorShape(Math.max(0.0001, Math.min(0.9998, input.pointerPosition.x / overlay.width)),
                    Math.max(0.0001, Math.min(0.9998, input.pointerPosition.y / overlay.height)), overlay.outputAspectRatio)
            requestPaint()
        }
        x: input.pointerPosition.x - width / 2
        y: input.pointerPosition.y - height / 2
        width: Math.min(2048, Math.max(6, Math.ceil(2 * pressureScale * overlay.width * Math.hypot(tip.ux || 0, tip.vx || 0) + 6)))
        height: Math.min(2048, Math.max(6, Math.ceil(2 * pressureScale * overlay.height * Math.hypot(tip.uy || 0, tip.vy || 0) + 6)))
        visible: input.containsPointer && !overlay.paint.picking
        onVisibleChanged: updateTip()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (!visible || width <= 0 || height <= 0) return
            const tip = cursor.tip
            if (tip.ux === undefined) return
            const pressureScale = cursor.pressureScale
            ctx.beginPath()
            for (let i = 0; i <= 48; ++i) {
                const a = i * 2 * Math.PI / 48
                const x = width / 2 + pressureScale * overlay.width * (tip.ux * Math.cos(a) + tip.vx * Math.sin(a))
                const y = height / 2 + pressureScale * overlay.height * (tip.uy * Math.cos(a) + tip.vy * Math.sin(a))
                if (i === 0) ctx.moveTo(x,y); else ctx.lineTo(x,y)
            }
            ctx.closePath(); ctx.lineWidth = 3; ctx.strokeStyle = "#80000000"; ctx.stroke()
            ctx.lineWidth = 1; ctx.strokeStyle = "#e0ffffff"; ctx.stroke()
        }
    }
}
