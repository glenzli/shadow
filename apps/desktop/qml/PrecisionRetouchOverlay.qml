pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick

// Direct manipulation for photo-local deterministic repair. Target and clone
// source coordinates stay in the recipe/controller; this overlay only maps
// level-zero geometry to the visible photo surface.
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
                color: Theme.transparent
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

                Rectangle {
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    radius: 9
                    color: Theme.previewHudStrongOverlay
                    border.width: 1
                    border.color: Theme.previewHudBorder

                    Text {
                        anchors.centerIn: parent
                        text: String(Number(repairHandle.modelData.index) + 1)
                        color: Theme.previewCompareDivider
                        font.pixelSize: 9
                        font.bold: true
                    }
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
                color: Theme.transparent
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
