pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick

// Direct crop manipulation over the complete oriented source. The controller
// owns normalized source-space geometry and undo/autosave; this component owns
// only the visual-space transform and one pointer gesture.
Item {
    id: overlay

    required property var editor
    required property bool interactionEnabled
    required property real aspectRatioLock

    readonly property var geometry: editor.photoGeometry
    readonly property int quarterTurn: Number(geometry.quarterTurn || 0)
    readonly property bool flipHorizontal: Boolean(geometry.flipHorizontal)
    readonly property bool flipVertical: Boolean(geometry.flipVertical)
    readonly property rect displayCrop: sourceBoundsToDisplay(
        Number(geometry.cropLeft),
        Number(geometry.cropTop),
        Number(geometry.cropRight),
        Number(geometry.cropBottom)
    )
    readonly property var handleRoles: [
        "topLeft", "top", "topRight", "right",
        "bottomRight", "bottom", "bottomLeft", "left"
    ]
    readonly property bool busy: Boolean(editor.stateBusy)

    visible: interactionEnabled && editor.active
    // Keep the cursor owner alive while editing is temporarily locked. The
    // gesture surfaces below are disabled independently, so a busy transition
    // cannot turn the pointer into an unexplained disappearance.
    enabled: visible

    function clamp01(value) {
        return Math.max(0, Math.min(1, value))
    }

    function sourcePointToDisplay(x, y) {
        let transformedX = flipHorizontal ? 1 - x : x
        let transformedY = flipVertical ? 1 - y : y
        if (quarterTurn === 1)
            return Qt.point(1 - transformedY, transformedX)
        if (quarterTurn === 2)
            return Qt.point(1 - transformedX, 1 - transformedY)
        if (quarterTurn === 3)
            return Qt.point(transformedY, 1 - transformedX)
        return Qt.point(transformedX, transformedY)
    }

    function displayPointToSource(x, y) {
        let sourceX = x
        let sourceY = y
        if (quarterTurn === 1) {
            sourceX = y
            sourceY = 1 - x
        } else if (quarterTurn === 2) {
            sourceX = 1 - x
            sourceY = 1 - y
        } else if (quarterTurn === 3) {
            sourceX = 1 - y
            sourceY = x
        }
        if (flipHorizontal)
            sourceX = 1 - sourceX
        if (flipVertical)
            sourceY = 1 - sourceY
        return Qt.point(clamp01(sourceX), clamp01(sourceY))
    }

    function sourceBoundsToDisplay(left, top, right, bottom) {
        const points = [
            sourcePointToDisplay(left, top),
            sourcePointToDisplay(right, top),
            sourcePointToDisplay(left, bottom),
            sourcePointToDisplay(right, bottom)
        ]
        let minimumX = 1
        let minimumY = 1
        let maximumX = 0
        let maximumY = 0
        for (let index = 0; index < points.length; ++index) {
            minimumX = Math.min(minimumX, points[index].x)
            minimumY = Math.min(minimumY, points[index].y)
            maximumX = Math.max(maximumX, points[index].x)
            maximumY = Math.max(maximumY, points[index].y)
        }
        return Qt.rect(minimumX, minimumY,
                       maximumX - minimumX, maximumY - minimumY)
    }

    function commitDisplayBounds(left, top, right, bottom) {
        const points = [
            displayPointToSource(left, top),
            displayPointToSource(right, top),
            displayPointToSource(left, bottom),
            displayPointToSource(right, bottom)
        ]
        let minimumX = 1
        let minimumY = 1
        let maximumX = 0
        let maximumY = 0
        for (let index = 0; index < points.length; ++index) {
            minimumX = Math.min(minimumX, points[index].x)
            minimumY = Math.min(minimumY, points[index].y)
            maximumX = Math.max(maximumX, points[index].x)
            maximumY = Math.max(maximumY, points[index].y)
        }
        editor.setPhotoCropBounds(
            minimumX, minimumY, maximumX, maximumY)
    }

    function cursorForRole(role) {
        if (role === "topLeft" || role === "bottomRight")
            return Qt.SizeFDiagCursor
        if (role === "topRight" || role === "bottomLeft")
            return Qt.SizeBDiagCursor
        if (role === "top" || role === "bottom")
            return Qt.SizeVerCursor
        return Qt.SizeHorCursor
    }

    function resizeBounds(role, origin, dx, dy) {
        const minimumExtent = 0.02
        let left = origin.x
        let top = origin.y
        let right = origin.x + origin.width
        let bottom = origin.y + origin.height

        if (role.indexOf("Left") >= 0 || role === "left")
            left = Math.min(right - minimumExtent, clamp01(left + dx))
        if (role.indexOf("Right") >= 0 || role === "right")
            right = Math.max(left + minimumExtent, clamp01(right + dx))
        if (role.indexOf("top") === 0)
            top = Math.min(bottom - minimumExtent, clamp01(top + dy))
        if (role.indexOf("bottom") === 0)
            bottom = Math.max(top + minimumExtent, clamp01(bottom + dy))

        if (aspectRatioLock > 0) {
            const normalizedRatio = aspectRatioLock
                * Math.max(1, overlay.height) / Math.max(1, overlay.width)
            const horizontalHandle = role === "left" || role === "right"
            const verticalHandle = role === "top" || role === "bottom"
            if (horizontalHandle) {
                const targetHeight = (right - left) / normalizedRatio
                const centerY = origin.y + origin.height / 2
                top = centerY - targetHeight / 2
                bottom = centerY + targetHeight / 2
            } else if (verticalHandle) {
                const targetWidth = (bottom - top) * normalizedRatio
                const centerX = origin.x + origin.width / 2
                left = centerX - targetWidth / 2
                right = centerX + targetWidth / 2
            } else {
                const width = right - left
                const height = bottom - top
                if (width / Math.max(minimumExtent, height) > normalizedRatio) {
                    const targetHeight = width / normalizedRatio
                    if (role.indexOf("top") === 0)
                        top = bottom - targetHeight
                    else
                        bottom = top + targetHeight
                } else {
                    const targetWidth = height * normalizedRatio
                    if (role.indexOf("Left") >= 0)
                        left = right - targetWidth
                    else
                        right = left + targetWidth
                }
            }

            const shiftX = left < 0 ? -left : right > 1 ? 1 - right : 0
            const shiftY = top < 0 ? -top : bottom > 1 ? 1 - bottom : 0
            left += shiftX
            right += shiftX
            top += shiftY
            bottom += shiftY
        }
        return Qt.rect(
            clamp01(left),
            clamp01(top),
            clamp01(right) - clamp01(left),
            clamp01(bottom) - clamp01(top)
        )
    }

    Rectangle {
        x: 0
        y: 0
        width: parent.width
        height: overlay.displayCrop.y * parent.height
        color: "#99000000"
    }
    Rectangle {
        x: 0
        y: (overlay.displayCrop.y + overlay.displayCrop.height) * parent.height
        width: parent.width
        height: Math.max(0, parent.height - y)
        color: "#99000000"
    }
    Rectangle {
        x: 0
        y: overlay.displayCrop.y * parent.height
        width: overlay.displayCrop.x * parent.width
        height: overlay.displayCrop.height * parent.height
        color: "#99000000"
    }
    Rectangle {
        x: (overlay.displayCrop.x + overlay.displayCrop.width) * parent.width
        y: overlay.displayCrop.y * parent.height
        width: Math.max(0, parent.width - x)
        height: overlay.displayCrop.height * parent.height
        color: "#99000000"
    }

    MouseArea {
        objectName: "cropSurfaceCursor"
        anchors.fill: parent
        z: overlay.busy ? 10 : 0
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        cursorShape: overlay.busy ? Qt.BusyCursor : Qt.CrossCursor
    }

    Item {
        id: cropFrame
        enabled: !overlay.busy
        x: overlay.displayCrop.x * overlay.width
        y: overlay.displayCrop.y * overlay.height
        width: overlay.displayCrop.width * overlay.width
        height: overlay.displayCrop.height * overlay.height

        Rectangle {
            anchors.fill: parent
            color: Theme.transparent
            border.width: 1
            border.color: Theme.previewCompareDivider
        }

        Repeater {
            model: [1 / 3, 2 / 3]
            delegate: Rectangle {
                required property real modelData
                x: Math.round(cropFrame.width * modelData)
                width: 1
                height: cropFrame.height
                color: "#88ffffff"
            }
        }
        Repeater {
            model: [1 / 3, 2 / 3]
            delegate: Rectangle {
                required property real modelData
                y: Math.round(cropFrame.height * modelData)
                width: cropFrame.width
                height: 1
                color: "#88ffffff"
            }
        }

        MouseArea {
            anchors.fill: parent
            anchors.margins: 10
            cursorShape: Qt.SizeAllCursor
            preventStealing: true
            property point pressPoint
            property rect origin
            onPressed: mouse => {
                pressPoint = Qt.point(mouse.x, mouse.y)
                origin = overlay.displayCrop
                overlay.editor.beginParameterEdit("geometry/crop/bounds")
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const dx = (mouse.x - pressPoint.x) / Math.max(1, overlay.width)
                const dy = (mouse.y - pressPoint.y) / Math.max(1, overlay.height)
                const left = Math.max(0, Math.min(
                    1 - origin.width, origin.x + dx))
                const top = Math.max(0, Math.min(
                    1 - origin.height, origin.y + dy))
                overlay.commitDisplayBounds(
                    left, top, left + origin.width, top + origin.height)
            }
            onReleased: overlay.editor.endParameterEdit("geometry/crop/bounds")
            onCanceled: overlay.editor.endParameterEdit("geometry/crop/bounds")
        }
    }

    Repeater {
        model: overlay.handleRoles

        delegate: Rectangle {
            id: handle
            required property string modelData
            enabled: !overlay.busy
            readonly property bool leftRole:
                modelData.indexOf("Left") >= 0 || modelData === "left"
            readonly property bool rightRole:
                modelData.indexOf("Right") >= 0 || modelData === "right"
            readonly property bool topRole:
                modelData.indexOf("top") === 0
            readonly property bool bottomRole:
                modelData.indexOf("bottom") === 0

            width: leftRole || rightRole
                ? (topRole || bottomRole ? 12 : 6) : 22
            height: topRole || bottomRole
                ? (leftRole || rightRole ? 12 : 6) : 22
            radius: 2
            x: (leftRole ? overlay.displayCrop.x
                : rightRole
                    ? overlay.displayCrop.x + overlay.displayCrop.width
                    : overlay.displayCrop.x + overlay.displayCrop.width / 2)
                * overlay.width - width / 2
            y: (topRole ? overlay.displayCrop.y
                : bottomRole
                    ? overlay.displayCrop.y + overlay.displayCrop.height
                    : overlay.displayCrop.y + overlay.displayCrop.height / 2)
                * overlay.height - height / 2
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.accent
            z: 3

            MouseArea {
                anchors.fill: parent
                anchors.margins: -8
                cursorShape: overlay.cursorForRole(handle.modelData)
                preventStealing: true
                property point pressPoint
                property rect origin
                onPressed: mouse => {
                    const point = handle.mapToItem(
                        overlay, mouse.x, mouse.y)
                    pressPoint = Qt.point(point.x, point.y)
                    origin = overlay.displayCrop
                    overlay.editor.beginParameterEdit("geometry/crop/bounds")
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const point = handle.mapToItem(
                        overlay, mouse.x, mouse.y)
                    const bounds = overlay.resizeBounds(
                        handle.modelData,
                        origin,
                        (point.x - pressPoint.x) / Math.max(1, overlay.width),
                        (point.y - pressPoint.y) / Math.max(1, overlay.height)
                    )
                    overlay.commitDisplayBounds(
                        bounds.x, bounds.y,
                        bounds.x + bounds.width,
                        bounds.y + bounds.height)
                }
                onReleased:
                    overlay.editor.endParameterEdit("geometry/crop/bounds")
                onCanceled:
                    overlay.editor.endParameterEdit("geometry/crop/bounds")
            }
        }
    }
}
