pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Owns target/donor coverage and donor drag lifecycle for one continuous repair stroke.
Item {
    id: strokeHandle

    required property var editor
    required property var modelData
    required property real pixelScale
    required property bool selected

    signal selectedRequested()

    property bool targetGestureActive: false
    property real targetLastX: 0
    property real targetLastY: 0
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
    readonly property real sourceRadiusPixels: radiusPixels * Math.max(
        0.25, Number(modelData.sourceScale)
    )
    readonly property real maximumSourceOffsetPixels: Math.max(
        0,
        (511 - Number(modelData.radius)
            * Math.max(0.25, Number(modelData.sourceScale))) * pixelScale
    )
    readonly property real authoredSourceOffsetX:
        Number(modelData.sourceOffsetX) * radiusPixels
    readonly property real authoredSourceOffsetY:
        Number(modelData.sourceOffsetY) * radiusPixels
    readonly property real sourceOffsetX:
        clampSourceOffsetPixels(authoredSourceOffsetX, true)
    readonly property real sourceOffsetY:
        clampSourceOffsetPixels(authoredSourceOffsetY, false)
    readonly property real sourceAnchorX:
        (pointBounds(true).x + pointBounds(true).y) / 2
    readonly property real sourceAnchorY:
        (pointBounds(false).x + pointBounds(false).y) / 2

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
        const angle = -Number(modelData.sourceRotation) * Math.PI / 180
        const cosine = Math.cos(angle)
        const sine = Math.sin(angle)
        const translatedX = x - sourceAnchorX - sourceOffsetX
        const translatedY = y - sourceAnchorY - sourceOffsetY
        const rotatedX = cosine * translatedX - sine * translatedY
        const rotatedY = sine * translatedX + cosine * translatedY
        const scale = Math.max(0.25, Number(modelData.sourceScale))
        const targetX = sourceAnchorX
            + rotatedX / (scale
                * (Boolean(modelData.sourceFlipHorizontal) ? -1 : 1))
        const targetY = sourceAnchorY
            + rotatedY / (scale
                * (Boolean(modelData.sourceFlipVertical) ? -1 : 1))
        return coverageContains(targetX, targetY, 0, 0)
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

    function transformedSourceBounds(horizontal) {
        if (points.length === 0)
            return Qt.point(0, 0)
        const angle = Number(modelData.sourceRotation) * Math.PI / 180
        const cosine = Math.cos(angle)
        const sine = Math.sin(angle)
        const scale = Math.max(0.25, Number(modelData.sourceScale))
        const flipX = Boolean(modelData.sourceFlipHorizontal) ? -1 : 1
        const flipY = Boolean(modelData.sourceFlipVertical) ? -1 : 1
        let lower = Number.POSITIVE_INFINITY
        let upper = Number.NEGATIVE_INFINITY
        for (let index = 0; index < points.length; ++index) {
            const deltaX = Number(points[index].x) * width - sourceAnchorX
            const deltaY = Number(points[index].y) * height - sourceAnchorY
            const transformedX = sourceAnchorX
                + scale * (cosine * flipX * deltaX
                    - sine * flipY * deltaY)
            const transformedY = sourceAnchorY
                + scale * (sine * flipX * deltaX
                    + cosine * flipY * deltaY)
            const value = horizontal ? transformedX : transformedY
            lower = Math.min(lower, value - sourceRadiusPixels)
            upper = Math.max(upper, value + sourceRadiusPixels)
        }
        return Qt.point(lower, upper)
    }

    function clampSourceOffsetPixels(candidate, horizontal) {
        const extent = horizontal ? width : height
        const bounds = transformedSourceBounds(horizontal)
        const minimum = -bounds.x
        const maximum = extent - bounds.y
        if (minimum > maximum)
            return extent / 2 - (bounds.x + bounds.y) / 2
        return Math.max(
            Math.max(minimum, -maximumSourceOffsetPixels),
            Math.min(Math.min(maximum, maximumSourceOffsetPixels), candidate)
        )
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

    function finishTargetGesture() {
        if (!targetGestureActive)
            return
        targetGestureActive = false
        editor.endParameterEdit(
            "retouch/stroke/" + modelData.index + "/position")
    }

    function nudgeSource(horizontalPixels, verticalPixels) {
        const radius = Math.max(0.25, radiusPixels)
        editor.setRetouchStrokeSourceOffset(
            modelData.index,
            sourceOffsetX / radius + horizontalPixels / radius,
            sourceOffsetY / radius + verticalPixels / radius
        )
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
        sourceTransformEnabled: true
        sourceAnchorX: strokeHandle.sourceAnchorX
        sourceAnchorY: strokeHandle.sourceAnchorY
        sourceRotationDegrees: Number(strokeHandle.modelData.sourceRotation)
        sourceScale: Math.max(0.25, Number(strokeHandle.modelData.sourceScale))
        sourceFlipHorizontal: Boolean(strokeHandle.modelData.sourceFlipHorizontal)
        sourceFlipVertical: Boolean(strokeHandle.modelData.sourceFlipVertical)
        coverageColor: Qt.rgba(
            Theme.previewCompareDivider.r,
            Theme.previewCompareDivider.g,
            Theme.previewCompareDivider.b,
            strokeHandle.selected ? 0.16 : 0.08
        )
    }

    Label {
        x: strokeHandle.sourceAnchorX + strokeHandle.sourceOffsetX - width / 2
        y: strokeHandle.sourceAnchorY + strokeHandle.sourceOffsetY - height - 8
        visible: strokeHandle.selected && strokeHandle.points.length > 0
        text: qsTr("SOURCE")
        color: Theme.accent
        font.pixelSize: 9
        font.bold: true
    }

    MouseArea {
        id: targetPointer
        objectName: "retouchStrokeTargetHitArea"

        readonly property point horizontalBounds: strokeHandle.pointBounds(true)
        readonly property point verticalBounds: strokeHandle.pointBounds(false)
        readonly property real hitRadius: Math.max(
            strokeHandle.interactionRadiusPixels,
            strokeHandle.radiusPixels + 3
        )

        x: horizontalBounds.x - hitRadius
        y: verticalBounds.x - hitRadius
        width: horizontalBounds.y - horizontalBounds.x + 2 * hitRadius
        height: verticalBounds.y - verticalBounds.x + 2 * hitRadius
        z: 3
        visible: strokeHandle.points.length > 0
        enabled: visible
        containmentMask: targetHitMask
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        propagateComposedEvents: false
        cursorShape: Qt.SizeAllCursor

        Item {
            id: targetHitMask
            anchors.fill: parent
            visible: false

            function contains(point) {
                return strokeHandle.targetContains(
                    point.x + targetPointer.x,
                    point.y + targetPointer.y
                )
            }
        }

        onPressed: mouse => {
            strokeHandle.selectTarget()
            const point = targetPointer.mapToItem(
                strokeHandle, mouse.x, mouse.y)
            strokeHandle.targetGestureActive = true
            strokeHandle.targetLastX = point.x
            strokeHandle.targetLastY = point.y
            strokeHandle.editor.beginParameterEdit(
                "retouch/stroke/" + strokeHandle.modelData.index
                    + "/position")
            mouse.accepted = true
        }
        onPositionChanged: mouse => {
            if (!pressed || !strokeHandle.targetGestureActive)
                return
            const point = targetPointer.mapToItem(
                strokeHandle, mouse.x, mouse.y)
            strokeHandle.editor.translateRetouchStroke(
                strokeHandle.modelData.index,
                (point.x - strokeHandle.targetLastX)
                    / Math.max(1, strokeHandle.width),
                (point.y - strokeHandle.targetLastY)
                    / Math.max(1, strokeHandle.height)
            )
            strokeHandle.targetLastX = point.x
            strokeHandle.targetLastY = point.y
        }
        onReleased: strokeHandle.finishTargetGesture()
        onCanceled: strokeHandle.finishTargetGesture()
    }

    MouseArea {
        id: sourcePointer
        objectName: "retouchStrokeSourceHitArea"

        readonly property point horizontalBounds:
            strokeHandle.transformedSourceBounds(true)
        readonly property point verticalBounds:
            strokeHandle.transformedSourceBounds(false)

        x: horizontalBounds.x + strokeHandle.sourceOffsetX
        y: verticalBounds.x + strokeHandle.sourceOffsetY
        width: horizontalBounds.y - horizontalBounds.x
        height: verticalBounds.y - verticalBounds.x
        // Preserve target selection at overlaps until the repair is selected;
        // the distinct unselected donor coverage is still directly clickable.
        z: strokeHandle.selected ? 4 : 2
        // The source coverage remains a direct selection target even before
        // its repair is selected. One press selects the repair and starts the
        // source gesture, including when source and target overlap.
        visible: strokeHandle.points.length > 0
        enabled: visible
        containmentMask: sourceHitMask
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        propagateComposedEvents: false
        cursorShape: Qt.CrossCursor
        focus: strokeHandle.selected

        Item {
            id: sourceHitMask
            anchors.fill: parent
            visible: false

            function contains(point) {
                return strokeHandle.sourceContains(
                    point.x + sourcePointer.x,
                    point.y + sourcePointer.y
                )
            }
        }

        Keys.onPressed: event => {
            const distance = (event.modifiers & Qt.ShiftModifier) !== 0
                ? 10 : 1
            if (event.key === Qt.Key_Left) {
                strokeHandle.nudgeSource(-distance, 0)
            } else if (event.key === Qt.Key_Right) {
                strokeHandle.nudgeSource(distance, 0)
            } else if (event.key === Qt.Key_Up) {
                strokeHandle.nudgeSource(0, -distance)
            } else if (event.key === Qt.Key_Down) {
                strokeHandle.nudgeSource(0, distance)
            } else {
                return
            }
            event.accepted = true
        }

        onPressed: mouse => {
            strokeHandle.selectedRequested()
            forceActiveFocus()
            const point = sourcePointer.mapToItem(
                strokeHandle, mouse.x, mouse.y)
            strokeHandle.sourceGestureActive = true
            strokeHandle.sourceStartX = point.x
            strokeHandle.sourceStartY = point.y
            strokeHandle.sourceStartOffsetX = Number(
                strokeHandle.sourceOffsetX
                    / Math.max(0.25, strokeHandle.radiusPixels))
            strokeHandle.sourceStartOffsetY = Number(
                strokeHandle.sourceOffsetY
                    / Math.max(0.25, strokeHandle.radiusPixels))
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
