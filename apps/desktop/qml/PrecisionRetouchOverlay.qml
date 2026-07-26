pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick

// Direct manipulation for photo-local deterministic repair. Legacy spots are
// still shown below, while a dragged repair is displayed as the same swept
// brush coverage that the renderer applies. This overlay deliberately shows
// regions, not authored point samples or centerline paths.
Item {
    id: overlay

    required property var editor
    required property bool interactionEnabled
    required property real levelZeroWidth
    required property real levelZeroHeight

    readonly property real pixelScale: Math.max(
        0.0001,
        Math.min(
            width / Math.max(1, levelZeroWidth),
            height / Math.max(1, levelZeroHeight)
        )
    )

    visible: interactionEnabled && editor.active
    enabled: visible && !editor.stateBusy

    function clamp01(value) {
        return Math.max(0, Math.min(1, value))
    }

    function finishTargetGesture(handle, key) {
        if (!handle.targetGestureActive)
            return
        handle.targetGestureActive = false
        editor.endParameterEdit(key)
    }

    Repeater {
        model: overlay.editor.retouchStrokes

        delegate: Item {
            id: strokeHandle

            required property var modelData
            property bool sourceGestureActive: false
            property real sourceStartX: 0
            property real sourceStartY: 0
            property real sourceStartOffsetX: 0
            property real sourceStartOffsetY: 0

            anchors.fill: parent
            z: 1

            readonly property var points: modelData.points || []
            readonly property real radiusPixels: Math.max(
                6,
                Number(modelData.radius) * overlay.pixelScale
            )
            readonly property bool cloneMode: Number(modelData.mode) === 1
            readonly property real sourceOffsetX:
                Number(modelData.sourceOffsetX) * radiusPixels
            readonly property real sourceOffsetY:
                Number(modelData.sourceOffsetY) * radiusPixels

            function bounds() {
                let left = Number.POSITIVE_INFINITY
                let top = Number.POSITIVE_INFINITY
                let right = Number.NEGATIVE_INFINITY
                let bottom = Number.NEGATIVE_INFINITY
                for (let pointIndex = 0;
                     pointIndex < points.length;
                     ++pointIndex) {
                    const point = points[pointIndex]
                    const pointX = Number(point.x) * width
                    const pointY = Number(point.y) * height
                    left = Math.min(left, pointX)
                    top = Math.min(top, pointY)
                    right = Math.max(right, pointX)
                    bottom = Math.max(bottom, pointY)
                }
                if (!Number.isFinite(left))
                    return { left: 0, top: 0, right: 0, bottom: 0 }
                return { left: left, top: top, right: right, bottom: bottom }
            }

            Canvas {
                id: targetCoverage
                anchors.fill: parent
                antialiasing: true
                property real coverageOffsetX: 0
                property real coverageOffsetY: 0
                property color coverageColor: Qt.rgba(
                    Theme.accent.r,
                    Theme.accent.g,
                    Theme.accent.b,
                    0.16
                )

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
                    for (let pointIndex = 0;
                         pointIndex < strokeHandle.points.length;
                         ++pointIndex) {
                        const point = strokeHandle.points[pointIndex]
                        const pointX = Number(point.x) * width + coverageOffsetX
                        const pointY = Number(point.y) * height + coverageOffsetY
                        if (previous === null) {
                            fillDisc(
                                context,
                                pointX,
                                pointY,
                                strokeHandle.radiusPixels
                            )
                        } else {
                            fillCapsule(
                                context,
                                previous.x,
                                previous.y,
                                pointX,
                                pointY,
                                strokeHandle.radiusPixels
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
                    target: overlay.editor
                    function onParametersChanged() {
                        targetCoverage.requestPaint()
                    }
                }
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onCoverageOffsetXChanged: requestPaint()
                onCoverageOffsetYChanged: requestPaint()
                Component.onCompleted: requestPaint()
            }

            Canvas {
                id: sourceCoverage
                anchors.fill: parent
                visible: strokeHandle.cloneMode
                antialiasing: true
                property real coverageOffsetX
                property real coverageOffsetY
                property color coverageColor
                coverageOffsetX: strokeHandle.sourceOffsetX
                coverageOffsetY: strokeHandle.sourceOffsetY
                coverageColor: Qt.rgba(
                    Theme.previewCompareDivider.r,
                    Theme.previewCompareDivider.g,
                    Theme.previewCompareDivider.b,
                    0.13
                )

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
                    for (let pointIndex = 0;
                         pointIndex < strokeHandle.points.length;
                         ++pointIndex) {
                        const point = strokeHandle.points[pointIndex]
                        const pointX = Number(point.x) * width + coverageOffsetX
                        const pointY = Number(point.y) * height + coverageOffsetY
                        if (previous === null) {
                            fillDisc(
                                context,
                                pointX,
                                pointY,
                                strokeHandle.radiusPixels
                            )
                        } else {
                            fillCapsule(
                                context,
                                previous.x,
                                previous.y,
                                pointX,
                                pointY,
                                strokeHandle.radiusPixels
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
                    target: overlay.editor
                    function onParametersChanged() {
                        sourceCoverage.requestPaint()
                    }
                }
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onCoverageOffsetXChanged: requestPaint()
                onCoverageOffsetYChanged: requestPaint()
                Component.onCompleted: requestPaint()
            }

            Item {
                id: sourceHitArea
                visible: strokeHandle.cloneMode && strokeHandle.points.length > 0
                readonly property var pathBounds: strokeHandle.bounds()
                x: pathBounds.left + strokeHandle.sourceOffsetX
                    - strokeHandle.radiusPixels
                y: pathBounds.top + strokeHandle.sourceOffsetY
                    - strokeHandle.radiusPixels
                width: Math.max(
                    strokeHandle.radiusPixels * 2,
                    pathBounds.right - pathBounds.left
                        + strokeHandle.radiusPixels * 2
                )
                height: Math.max(
                    strokeHandle.radiusPixels * 2,
                    pathBounds.bottom - pathBounds.top
                        + strokeHandle.radiusPixels * 2
                )
                z: 3

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: Qt.CrossCursor

                    onPressed: mouse => {
                        const point = sourceHitArea.mapToItem(
                            overlay, mouse.x, mouse.y)
                        strokeHandle.sourceGestureActive = true
                        strokeHandle.sourceStartX = point.x
                        strokeHandle.sourceStartY = point.y
                        strokeHandle.sourceStartOffsetX = Number(
                            strokeHandle.modelData.sourceOffsetX)
                        strokeHandle.sourceStartOffsetY = Number(
                            strokeHandle.modelData.sourceOffsetY)
                        overlay.editor.beginParameterEdit(
                            "retouch/stroke/" + strokeHandle.modelData.index
                                + "/source")
                    }
                    onPositionChanged: mouse => {
                        if (!pressed || !strokeHandle.sourceGestureActive)
                            return
                        const point = sourceHitArea.mapToItem(
                            overlay, mouse.x, mouse.y)
                        const radius = Math.max(1, strokeHandle.radiusPixels)
                        overlay.editor.setRetouchStrokeSourceOffset(
                            strokeHandle.modelData.index,
                            Math.max(-2, Math.min(2,
                                strokeHandle.sourceStartOffsetX
                                    + (point.x - strokeHandle.sourceStartX) / radius)),
                            Math.max(-2, Math.min(2,
                                strokeHandle.sourceStartOffsetY
                                    + (point.y - strokeHandle.sourceStartY) / radius))
                        )
                    }
                    onReleased: finishSourceGesture()
                    onCanceled: finishSourceGesture()

                    function finishSourceGesture() {
                        if (!strokeHandle.sourceGestureActive)
                            return
                        strokeHandle.sourceGestureActive = false
                        overlay.editor.endParameterEdit(
                            "retouch/stroke/" + strokeHandle.modelData.index
                                + "/source")
                    }
                }
            }
        }
    }

    Repeater {
        model: overlay.editor.retouchSpots

        delegate: Item {
            id: repairHandle

            required property var modelData
            property bool targetGestureActive: false
            property bool sourceGestureActive: false

            anchors.fill: parent
            z: 2

            readonly property real radiusPixels: Math.max(
                6,
                Number(modelData.radius) * overlay.pixelScale
            )
            readonly property real targetX: Number(modelData.x) * width
            readonly property real targetY: Number(modelData.y) * height
            readonly property real sourceX: targetX
                + Number(modelData.sourceOffsetX) * radiusPixels
            readonly property real sourceY: targetY
                + Number(modelData.sourceOffsetY) * radiusPixels
            readonly property bool cloneMode: Number(modelData.mode) === 1

            Canvas {
                id: connector
                anchors.fill: parent
                visible: repairHandle.cloneMode
                antialiasing: true

                onPaint: {
                    const context = getContext("2d")
                    context.clearRect(0, 0, width, height)
                    context.beginPath()
                    context.moveTo(repairHandle.targetX, repairHandle.targetY)
                    context.lineTo(repairHandle.sourceX, repairHandle.sourceY)
                    context.strokeStyle = Theme.previewHudStrongOverlay
                    context.lineWidth = 4
                    context.stroke()
                    context.beginPath()
                    context.moveTo(repairHandle.targetX, repairHandle.targetY)
                    context.lineTo(repairHandle.sourceX, repairHandle.sourceY)
                    context.strokeStyle = Theme.previewCompareDivider
                    context.lineWidth = 1
                    context.stroke()
                }

                Connections {
                    target: overlay.editor
                    function onParametersChanged() {
                        connector.requestPaint()
                    }
                }
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Component.onCompleted: requestPaint()
            }

            Rectangle {
                id: targetCircle
                x: repairHandle.targetX - width / 2
                y: repairHandle.targetY - height / 2
                width: repairHandle.radiusPixels * 2
                height: width
                radius: width / 2
                color: Qt.rgba(
                    Theme.accent.r,
                    Theme.accent.g,
                    Theme.accent.b,
                    0.16
                )
                border.width: 2
                border.color: Theme.accent

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width
                        * Math.max(0, 1 - Number(repairHandle.modelData.feather))
                    height: width
                    radius: width / 2
                    visible: width >= 4
                    color: Theme.transparent
                    border.width: 1
                    border.color: Theme.previewCompareDivider
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -5
                    acceptedButtons: Qt.LeftButton
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: Qt.SizeAllCursor
                    onPressed: {
                        repairHandle.targetGestureActive = true
                        overlay.editor.beginParameterEdit(
                            "retouch/" + repairHandle.modelData.index + "/center")
                    }
                    onPositionChanged: mouse => {
                        if (!pressed)
                            return
                        const point = targetCircle.mapToItem(
                            overlay, mouse.x, mouse.y)
                        overlay.editor.setRetouchSpotCenter(
                            repairHandle.modelData.index,
                            overlay.clamp01(point.x / Math.max(1, overlay.width)),
                            overlay.clamp01(point.y / Math.max(1, overlay.height))
                        )
                    }
                    onReleased: overlay.finishTargetGesture(
                        repairHandle,
                        "retouch/" + repairHandle.modelData.index + "/center")
                    onCanceled: overlay.finishTargetGesture(
                        repairHandle,
                        "retouch/" + repairHandle.modelData.index + "/center")
                }
            }

            Rectangle {
                id: sourceCircle
                visible: repairHandle.cloneMode
                x: repairHandle.sourceX - width / 2
                y: repairHandle.sourceY - height / 2
                width: repairHandle.radiusPixels * 2
                height: width
                radius: width / 2
                color: Qt.rgba(
                    Theme.accent.r,
                    Theme.accent.g,
                    Theme.accent.b,
                    0.12
                )
                border.width: 1
                border.color: Theme.previewCompareDivider

                Rectangle {
                    anchors.centerIn: parent
                    width: 8
                    height: 8
                    radius: 4
                    color: Theme.previewCompareDivider
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -5
                    acceptedButtons: Qt.LeftButton
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: Qt.CrossCursor
                    onPressed: {
                        repairHandle.sourceGestureActive = true
                        overlay.editor.beginParameterEdit(
                            "retouch/" + repairHandle.modelData.index + "/source")
                    }
                    onPositionChanged: mouse => {
                        if (!pressed)
                            return
                        const point = sourceCircle.mapToItem(
                            overlay, mouse.x, mouse.y)
                        const radius = Math.max(1, repairHandle.radiusPixels)
                        overlay.editor.setRetouchSpotSourceOffset(
                            repairHandle.modelData.index,
                            Math.max(-2, Math.min(
                                2, (point.x - repairHandle.targetX) / radius)),
                            Math.max(-2, Math.min(
                                2, (point.y - repairHandle.targetY) / radius))
                        )
                    }
                    onReleased: {
                        if (!repairHandle.sourceGestureActive)
                            return
                        repairHandle.sourceGestureActive = false
                        overlay.editor.endParameterEdit(
                            "retouch/" + repairHandle.modelData.index + "/source")
                    }
                    onCanceled: {
                        if (!repairHandle.sourceGestureActive)
                            return
                        repairHandle.sourceGestureActive = false
                        overlay.editor.endParameterEdit(
                            "retouch/" + repairHandle.modelData.index + "/source")
                    }
                }
            }
        }
    }
}
