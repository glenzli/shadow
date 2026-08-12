pragma ComponentBehavior: Bound

import QtQuick

// Owns target/donor coverage and donor drag lifecycle for one continuous repair stroke.
Item {
    id: strokeHandle

    required property var editor
    required property var modelData
    required property real pixelScale
    required property bool selected

    signal selectedRequested()

    property bool sourceGestureActive: false
    property real sourceStartX: 0
    property real sourceStartY: 0
    property real sourceStartOffsetX: 0
    property real sourceStartOffsetY: 0

    readonly property var points: modelData.points || []
    readonly property real radiusPixels: Math.max(
        0.25,
        Number(modelData.radius) * pixelScale
    )
    readonly property real interactionRadiusPixels: Math.max(6, radiusPixels)
    readonly property real sourceOffsetX:
        Number(modelData.sourceOffsetX) * radiusPixels
    readonly property real sourceOffsetY:
        Number(modelData.sourceOffsetY) * radiusPixels

    function squaredDistanceToSegment(
        x, y, startX, startY, endX, endY
    ) {
        const deltaX = endX - startX
        const deltaY = endY - startY
        const lengthSquared = deltaX * deltaX + deltaY * deltaY
        if (lengthSquared <= 0.0001) {
            const pointDeltaX = x - startX
            const pointDeltaY = y - startY
            return pointDeltaX * pointDeltaX + pointDeltaY * pointDeltaY
        }
        const projection = Math.max(0, Math.min(
            1,
            ((x - startX) * deltaX + (y - startY) * deltaY)
                / lengthSquared
        ))
        const closestX = startX + projection * deltaX
        const closestY = startY + projection * deltaY
        const pointDeltaX = x - closestX
        const pointDeltaY = y - closestY
        return pointDeltaX * pointDeltaX + pointDeltaY * pointDeltaY
    }

    function coverageContains(x, y, offsetX, offsetY) {
        if (points.length === 0)
            return false
        const hitRadius = Math.max(
            interactionRadiusPixels,
            radiusPixels + 3
        )
        const maximumDistanceSquared = hitRadius * hitRadius
        let previous = null
        for (let pointIndex = 0; pointIndex < points.length; ++pointIndex) {
            const point = points[pointIndex]
            const pointX = Number(point.x) * width + offsetX
            const pointY = Number(point.y) * height + offsetY
            if (squaredDistanceToSegment(
                    x, y, pointX, pointY, pointX, pointY
                ) <= maximumDistanceSquared) {
                return true
            }
            if (previous !== null && !Boolean(point.beginsStroke)) {
                const previousX = Number(previous.x) * width + offsetX
                const previousY = Number(previous.y) * height + offsetY
                if (squaredDistanceToSegment(
                        x, y, previousX, previousY, pointX, pointY
                    ) <= maximumDistanceSquared) {
                    return true
                }
            }
            previous = point
        }
        return false
    }

    function targetContains(x, y) {
        return coverageContains(x, y, 0, 0)
    }

    function sourceContains(x, y) {
        return coverageContains(x, y, sourceOffsetX, sourceOffsetY)
    }

    function pointBounds(horizontal) {
        if (points.length === 0)
            return Qt.point(0, 0)
        let lower = Number(horizontal ? points[0].x : points[0].y)
            * (horizontal ? width : height)
        let upper = lower
        for (let index = 1; index < points.length; ++index) {
            const value = Number(horizontal ? points[index].x : points[index].y)
                * (horizontal ? width : height)
            lower = Math.min(lower, value)
            upper = Math.max(upper, value)
        }
        return Qt.point(lower, upper)
    }

    function clampSourceOffsetPixels(candidate, horizontal) {
        const extent = horizontal ? width : height
        const bounds = pointBounds(horizontal)
        const minimum = radiusPixels - bounds.x
        const maximum = extent - radiusPixels - bounds.y
        if (minimum > maximum)
            return extent / 2 - (bounds.x + bounds.y) / 2
        return Math.max(minimum, Math.min(maximum, candidate))
    }

    function selectTarget() {
        selectedRequested()
    }

    function finishSourceGesture() {
        if (!sourceGestureActive)
            return
        sourceGestureActive = false
        editor.endParameterEdit(
            "retouch/stroke/" + modelData.index + "/source")
    }

    PrecisionRetouchStrokeCoverage {
        anchors.fill: parent
        editor: strokeHandle.editor
        points: strokeHandle.points
        radiusPixels: strokeHandle.radiusPixels
        coverageColor: Qt.rgba(
            Theme.accent.r,
            Theme.accent.g,
            Theme.accent.b,
            strokeHandle.selected ? 0.22 : 0.11
        )
    }

    PrecisionRetouchStrokeCoverage {
        anchors.fill: parent
        editor: strokeHandle.editor
        points: strokeHandle.points
        radiusPixels: strokeHandle.radiusPixels
        coverageOffsetX: strokeHandle.sourceOffsetX
        coverageOffsetY: strokeHandle.sourceOffsetY
        coverageColor: Qt.rgba(
            Theme.previewCompareDivider.r,
            Theme.previewCompareDivider.g,
            Theme.previewCompareDivider.b,
            strokeHandle.selected ? 0.16 : 0.08
        )
    }

    Item {
        id: targetHitMask
        anchors.fill: parent
        visible: false

        function contains(point) {
            return strokeHandle.targetContains(point.x, point.y)
        }
    }

    Item {
        id: sourceHitMask
        anchors.fill: parent
        visible: false

        function contains(point) {
            return strokeHandle.sourceContains(point.x, point.y)
        }
    }

    MouseArea {
        id: targetPointer
        objectName: "retouchStrokeTargetHitArea"

        anchors.fill: parent
        z: 3
        visible: strokeHandle.points.length > 0
        enabled: visible
        containmentMask: targetHitMask
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        propagateComposedEvents: false
        cursorShape: Qt.PointingHandCursor

        onPressed: mouse => {
            strokeHandle.selectTarget()
            mouse.accepted = true
        }
    }

    MouseArea {
        id: sourcePointer
        objectName: "retouchStrokeSourceHitArea"

        anchors.fill: parent
        z: 4
        // Donor manipulation belongs to the selected repair. This keeps
        // unselected donor overlays from stealing a target selection or a
        // new paint gesture. When donor and target overlap, the selected
        // donor remains above the target and therefore keeps drag priority.
        visible: strokeHandle.selected && strokeHandle.points.length > 0
        enabled: visible
        containmentMask: sourceHitMask
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        propagateComposedEvents: false
        cursorShape: Qt.CrossCursor

        onPressed: mouse => {
            strokeHandle.selectedRequested()
            const point = sourcePointer.mapToItem(
                strokeHandle, mouse.x, mouse.y)
            strokeHandle.sourceGestureActive = true
            strokeHandle.sourceStartX = point.x
            strokeHandle.sourceStartY = point.y
            strokeHandle.sourceStartOffsetX = Number(
                strokeHandle.modelData.sourceOffsetX)
            strokeHandle.sourceStartOffsetY = Number(
                strokeHandle.modelData.sourceOffsetY)
            strokeHandle.editor.beginParameterEdit(
                "retouch/stroke/" + strokeHandle.modelData.index
                    + "/source")
            mouse.accepted = true
        }
        onPositionChanged: mouse => {
            if (!pressed || !strokeHandle.sourceGestureActive)
                return
            const point = sourcePointer.mapToItem(
                strokeHandle, mouse.x, mouse.y)
            const radius = Math.max(0.25, strokeHandle.radiusPixels)
            const candidateOffsetX =
                strokeHandle.sourceStartOffsetX * radius
                + point.x - strokeHandle.sourceStartX
            const candidateOffsetY =
                strokeHandle.sourceStartOffsetY * radius
                + point.y - strokeHandle.sourceStartY
            strokeHandle.editor.setRetouchStrokeSourceOffset(
                strokeHandle.modelData.index,
                strokeHandle.clampSourceOffsetPixels(
                    candidateOffsetX, true) / radius,
                strokeHandle.clampSourceOffsetPixels(
                    candidateOffsetY, false) / radius
            )
        }
        onReleased: strokeHandle.finishSourceGesture()
        onCanceled: strokeHandle.finishSourceGesture()
    }
}
