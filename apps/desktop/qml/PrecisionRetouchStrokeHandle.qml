pragma ComponentBehavior: Bound

import QtQuick

// Owns the target/source coverage and clone-source drag lifecycle for one continuous stroke.
Item {
    id: strokeHandle

    required property var editor
    required property var modelData
    required property real pixelScale

    property bool sourceGestureActive: false
    property real sourceStartX: 0
    property real sourceStartY: 0
    property real sourceStartOffsetX: 0
    property real sourceStartOffsetY: 0

    readonly property var points: modelData.points || []
    readonly property real radiusPixels: Math.max(
        6,
        Number(modelData.radius) * pixelScale
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
        for (let pointIndex = 0; pointIndex < points.length; ++pointIndex) {
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
            0.16
        )
    }

    PrecisionRetouchStrokeCoverage {
        anchors.fill: parent
        visible: strokeHandle.cloneMode
        editor: strokeHandle.editor
        points: strokeHandle.points
        radiusPixels: strokeHandle.radiusPixels
        coverageOffsetX: strokeHandle.sourceOffsetX
        coverageOffsetY: strokeHandle.sourceOffsetY
        coverageColor: Qt.rgba(
            Theme.previewCompareDivider.r,
            Theme.previewCompareDivider.g,
            Theme.previewCompareDivider.b,
            0.13
        )
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
            }
            onPositionChanged: mouse => {
                if (!pressed || !strokeHandle.sourceGestureActive)
                    return
                const point = sourceHitArea.mapToItem(
                    strokeHandle, mouse.x, mouse.y)
                const radius = Math.max(1, strokeHandle.radiusPixels)
                strokeHandle.editor.setRetouchStrokeSourceOffset(
                    strokeHandle.modelData.index,
                    Math.max(-2, Math.min(
                        2,
                        strokeHandle.sourceStartOffsetX
                            + (point.x - strokeHandle.sourceStartX) / radius
                    )),
                    Math.max(-2, Math.min(
                        2,
                        strokeHandle.sourceStartOffsetY
                            + (point.y - strokeHandle.sourceStartY) / radius
                    ))
                )
            }
            onReleased: strokeHandle.finishSourceGesture()
            onCanceled: strokeHandle.finishSourceGesture()
        }
    }
}
