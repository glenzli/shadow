pragma ComponentBehavior: Bound

import QtQuick

// Owns the target/source visualization and drag lifecycle for one legacy point repair.
Item {
    id: repairHandle

    required property var editor
    required property var modelData
    required property real pixelScale
    required property bool selected

    signal selectedRequested()

    property bool targetGestureActive: false
    property bool sourceGestureActive: false

    readonly property real radiusPixels: Math.max(
        6,
        Number(modelData.radius) * pixelScale
    )
    readonly property real targetX: Number(modelData.x) * width
    readonly property real targetY: Number(modelData.y) * height
    readonly property real sourceX: targetX
        + Number(modelData.sourceOffsetX) * radiusPixels
    readonly property real sourceY: targetY
        + Number(modelData.sourceOffsetY) * radiusPixels
    function clamp01(value) {
        return Math.max(0, Math.min(1, value))
    }

    function finishTargetGesture() {
        if (!targetGestureActive)
            return
        targetGestureActive = false
        editor.endParameterEdit("retouch/" + modelData.index + "/center")
    }

    function finishSourceGesture() {
        if (!sourceGestureActive)
            return
        sourceGestureActive = false
        editor.endParameterEdit("retouch/" + modelData.index + "/source")
    }

    Canvas {
        id: connector

        anchors.fill: parent
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
            target: repairHandle.editor
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

        z: 1
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
        border.color: repairHandle.selected
            ? Theme.accent : Theme.previewCompareDivider

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
            objectName: "retouchSpotTargetHitArea"

            anchors.fill: parent
            anchors.margins: -5
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeAllCursor

            onPressed: {
                repairHandle.selectedRequested()
                repairHandle.targetGestureActive = true
                repairHandle.editor.beginParameterEdit(
                    "retouch/" + repairHandle.modelData.index + "/center")
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const point = targetCircle.mapToItem(
                    repairHandle, mouse.x, mouse.y)
                repairHandle.editor.setRetouchSpotCenter(
                    repairHandle.modelData.index,
                    repairHandle.clamp01(
                        point.x / Math.max(1, repairHandle.width)),
                    repairHandle.clamp01(
                        point.y / Math.max(1, repairHandle.height))
                )
            }
            onReleased: repairHandle.finishTargetGesture()
            onCanceled: repairHandle.finishTargetGesture()
        }
    }

    Rectangle {
        id: sourceCircle

        z: 2
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
        border.color: repairHandle.selected
            ? Theme.accent : Theme.previewCompareDivider

        Rectangle {
            anchors.centerIn: parent
            width: 8
            height: 8
            radius: 4
            color: Theme.previewCompareDivider
        }

        MouseArea {
            objectName: "retouchSpotSourceHitArea"

            anchors.fill: parent
            anchors.margins: -5
            // Keep every donor visible, but only the selected repair owns
            // donor input. Unselected donors must not steal target selection
            // or a new paint gesture from the lower canvas input.
            enabled: repairHandle.selected
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.CrossCursor

            onPressed: {
                repairHandle.selectedRequested()
                repairHandle.sourceGestureActive = true
                repairHandle.editor.beginParameterEdit(
                    "retouch/" + repairHandle.modelData.index + "/source")
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const point = sourceCircle.mapToItem(
                    repairHandle, mouse.x, mouse.y)
                const radius = Math.max(1, repairHandle.radiusPixels)
                repairHandle.editor.setRetouchSpotSourceOffset(
                    repairHandle.modelData.index,
                    Math.max(-8, Math.min(
                        8, (point.x - repairHandle.targetX) / radius)),
                    Math.max(-8, Math.min(
                        8, (point.y - repairHandle.targetY) / radius))
                )
            }
            onReleased: repairHandle.finishSourceGesture()
            onCanceled: repairHandle.finishSourceGesture()
        }
    }
}
