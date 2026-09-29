pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// Geometry-aware handles share the renderer's source-coordinate map. The
// centreline and local brush ellipse are projected; stored paths stay original.
Item {
    id: handle
    objectName: "projectedRetouchHandle"
    required property var editor
    required property var modelData
    required property bool continuous
    required property bool selected
    property var projection: ({})
    signal selectedRequested()
    function refresh() {
        if (!editor || !modelData || editor.retouchSources === undefined) return
        projection = editor.retouchSources.projectRegion(continuous, modelData.index, selected)
        outline.requestPaint()
    }
    function points(source) { return (source ? projection.source : projection.target) || [] }
    function key(source) {
        return (continuous ? "retouch/stroke/" : "retouch/") + modelData.index
            + (source ? "/source" : continuous ? "/position" : "/center")
    }
    function radius(p) {
        return Math.max(Math.hypot(p.ux * width, p.uy * height), Math.hypot(p.vx * width, p.vy * height))
    }
    function hit(source, x, y) {
        const path = points(source)
        for (let i = 0; i < path.length; ++i) {
            const a = path[i], b = path[Math.max(0, i - 1)]
            const ax = a.x * width, ay = a.y * height
            const dx = b.x * width - ax, dy = b.y * height - ay
            const t = Math.max(0, Math.min(1, ((x - ax) * dx + (y - ay) * dy) / Math.max(1e-12, dx * dx + dy * dy)))
            if (Math.hypot(x - ax - t * dx, y - ay - t * dy) <= Math.max(6, radius(a) + 3)) return true
        }
        return false
    }
    onModelDataChanged: refresh()
    onSelectedChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: handle.editor
        function onParametersChanged() { handle.refresh() }
    }
    Label {
        readonly property var sourcePoints: handle.points(true)
        visible: handle.selected && sourcePoints.length > 0
        x: sourcePoints.length ? sourcePoints[0].x * handle.width - width / 2 : 0
        y: sourcePoints.length ? sourcePoints[0].y * handle.height - handle.radius(sourcePoints[0]) - height - 3 : 0
        text: qsTranslate("PrecisionRetouchSpotHandle", "SOURCE")
        color: Theme.accent
        font.pixelSize: Theme.fontCaption
        font.bold: true
    }
    Canvas {
        id: outline
        anchors.fill: parent
        antialiasing: true
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            for (let source = 0; source < 2; ++source) {
                const path = handle.points(source !== 0)
                ctx.strokeStyle = handle.selected ? Theme.accent : Theme.previewCompareDivider
                ctx.fillStyle = Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, source ? 0.08 : 0.16)
                ctx.lineWidth = 1.5
                for (let i = 0; i < path.length; ++i) {
                    const p = path[i]
                    ctx.beginPath()
                    for (let step = 0; step <= 20; ++step) {
                        const angle = step * Math.PI / 10
                        const x = (p.x + p.ux * Math.cos(angle) + p.vx * Math.sin(angle)) * width
                        const y = (p.y + p.uy * Math.cos(angle) + p.vy * Math.sin(angle)) * height
                        if (step === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                    }
                    ctx.closePath(); ctx.fill(); ctx.stroke()
                    if (i > 0) {
                        ctx.beginPath(); ctx.moveTo(path[i - 1].x * width, path[i - 1].y * height)
                        ctx.lineTo(p.x * width, p.y * height); ctx.stroke()
                    }
                }
            }
            const a = handle.points(false), b = handle.points(true)
            if (a.length && b.length) {
                ctx.beginPath(); ctx.moveTo(a[0].x * width, a[0].y * height)
                ctx.lineTo(b[0].x * width, b[0].y * height); ctx.stroke()
            }
        }
    }
    Repeater {
        model: 2
        delegate: MouseArea {
            id: pointer
            required property int index
            readonly property bool source: index === 1
            property real lastX: 0
            property real lastY: 0
            property bool gesture: false
            anchors.fill: parent
            z: source && handle.selected ? 4 : source ? 1 : 2
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            preventStealing: true
            cursorShape: source ? Qt.CrossCursor : Qt.SizeAllCursor
            containmentMask: Item {
                function contains(p) { return handle.hit(pointer.source, p.x, p.y) }
            }
            onPressed: mouse => {
                handle.selectedRequested()
                lastX = mouse.x / width; lastY = mouse.y / height
                gesture = true
                handle.editor.beginParameterEdit(handle.key(source))
            }
            onPositionChanged: mouse => {
                if (!pressed || !gesture) return
                const x = mouse.x / width, y = mouse.y / height
                handle.editor.retouchSources.moveRegion(handle.continuous, handle.modelData.index, source, lastX, lastY, x, y)
                lastX = x; lastY = y
            }
            function finish() {
                if (!gesture) return
                gesture = false
                handle.editor.endParameterEdit(handle.key(source))
            }
            onReleased: finish()
            onCanceled: finish()
            Component.onDestruction: finish()
        }
    }
}
