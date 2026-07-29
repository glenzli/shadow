pragma ComponentBehavior: Bound

import QtQuick

// Direct-manipulation owner for preview sampling and repair-stroke authoring.
// The canvas supplies one generation-verified preview surface and its painted
// content rectangle; this component owns coordinate normalization, picker
// routing, stroke sampling, terminal
// gesture cleanup, and the pointer affordance as one interaction lifecycle.
Item {
    id: pickerInput

    required property var editor
    required property Item previewItem
    required property rect previewContentRect
    required property bool previewFrameReady
    required property string readyPreviewGeneration
    required property real displayScale
    required property bool interactionEnabled

    visible: interactionEnabled
    enabled: visible

    function normalizedPreviewPoint(sourceItem, sourceX, sourceY) {
        if (!previewFrameReady || readyPreviewGeneration.length === 0
                || previewContentRect.width <= 0
                || previewContentRect.height <= 0) {
            return null
        }
        const mapped = previewItem.mapFromItem(sourceItem, sourceX, sourceY)
        const paintedWidth = Math.max(1, previewContentRect.width)
        const paintedHeight = Math.max(1, previewContentRect.height)
        const paintedX = previewContentRect.x
        const paintedY = previewContentRect.y
        if (mapped.x < paintedX || mapped.y < paintedY
                || mapped.x > paintedX + paintedWidth
                || mapped.y > paintedY + paintedHeight) {
            return null
        }
        return Qt.point(
            Math.max(0, Math.min(1, (mapped.x - paintedX) / paintedWidth)),
            Math.max(0, Math.min(1, (mapped.y - paintedY) / paintedHeight))
        )
    }

    function pickPreviewColor(sourceItem, sourceX, sourceY) {
        const normalized = normalizedPreviewPoint(
            sourceItem, sourceX, sourceY)
        if (normalized === null)
            return
        if (editor.whiteBalancePickerActive) {
            editor.setWhiteBalanceFromPreview(
                normalized.x, normalized.y, readyPreviewGeneration)
        } else {
            editor.addPointColorFromPreview(
                normalized.x, normalized.y, readyPreviewGeneration)
        }
    }

    MouseArea {
        id: inputArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: enabled ? Qt.BlankCursor : Qt.ArrowCursor
        preventStealing: true

        property real pointerX: width / 2
        property real pointerY: height / 2
        property bool retouchGestureActive: false
        property bool retouchStrokeActive: false
        property real retouchPressX: -1
        property real retouchPressY: -1
        property real lastRetouchStrokeX: -1
        property real lastRetouchStrokeY: -1
        property var retouchPressPoint: null

        function retouchBrushDiameter() {
            // The persisted repair target has an 18px level-zero radius.
            // Match its visible diameter instead of creating a second
            // brush-size contract in presentation code.
            return Math.max(18, 36 * pickerInput.displayScale)
        }

        function appendRetouchStrokePoint(mouse, force) {
            const normalized = pickerInput.normalizedPreviewPoint(
                inputArea, mouse.x, mouse.y)
            if (normalized === null)
                return
            // Keep a compact sampled path while leaving the renderer
            // responsible for joining every adjacent pair into one swept
            // brush region.
            const minimumSpacing = Math.max(
                2, retouchBrushDiameter() * 0.18)
            if (!force
                    && Math.hypot(
                        mouse.x - lastRetouchStrokeX,
                        mouse.y - lastRetouchStrokeY
                    ) < minimumSpacing) {
                return
            }
            pickerInput.editor.appendRetouchStrokePoint(
                normalized.x, normalized.y)
            lastRetouchStrokeX = mouse.x
            lastRetouchStrokeY = mouse.y
        }

        function finishRetouchGesture(mouse) {
            if (!retouchGestureActive)
                return
            if (retouchStrokeActive) {
                if (mouse !== undefined && mouse !== null)
                    appendRetouchStrokePoint(mouse, true)
                pickerInput.editor.endRetouchStroke()
            } else if (retouchPressPoint !== null) {
                // A click remains a single legacy spot: existing recipes and
                // the precise spot workflow retain their original behavior.
                pickerInput.editor.addRetouchSpotFromPreview(
                    retouchPressPoint.x, retouchPressPoint.y)
            }
            retouchGestureActive = false
            retouchStrokeActive = false
            retouchPressPoint = null
            retouchPressX = -1
            retouchPressY = -1
            lastRetouchStrokeX = -1
            lastRetouchStrokeY = -1
        }

        onPositionChanged: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            if (!pressed || !retouchGestureActive)
                return
            if (!retouchStrokeActive) {
                const dragThreshold = Math.max(
                    3, retouchBrushDiameter() * 0.12)
                if (Math.hypot(
                        mouse.x - retouchPressX,
                        mouse.y - retouchPressY
                    ) < dragThreshold) {
                    return
                }
                pickerInput.editor.beginRetouchStroke(
                    retouchPressPoint.x, retouchPressPoint.y)
                retouchStrokeActive = true
                lastRetouchStrokeX = retouchPressX
                lastRetouchStrokeY = retouchPressY
            }
            appendRetouchStrokePoint(mouse, false)
        }

        onPressed: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            if (!pickerInput.editor.retouchPickerActive)
                return
            const normalized = pickerInput.normalizedPreviewPoint(
                inputArea, mouse.x, mouse.y)
            if (normalized === null)
                return
            retouchGestureActive = true
            retouchStrokeActive = false
            retouchPressX = mouse.x
            retouchPressY = mouse.y
            retouchPressPoint = normalized
        }
        onReleased: mouse => finishRetouchGesture(mouse)
        onCanceled: finishRetouchGesture(null)
        onClicked: mouse => {
            if (!pickerInput.editor.retouchPickerActive) {
                pickerInput.pickPreviewColor(
                    inputArea, mouse.x, mouse.y)
            }
        }
    }

    Item {
        visible: inputArea.containsMouse
        x: inputArea.pointerX
        y: inputArea.pointerY

        Rectangle {
            visible: pickerInput.editor.retouchPickerActive
            anchors.centerIn: parent
            width: Math.max(18, 36 * pickerInput.displayScale)
            height: width
            radius: width / 2
            color: Theme.transparent
            border.width: 1
            border.color: Theme.previewCompareDivider

            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                radius: width / 2
                color: Theme.transparent
                border.width: 1
                border.color: Theme.accent
            }
        }

        Item {
            visible: !pickerInput.editor.retouchPickerActive
            x: -6
            y: -19
            width: 24
            height: 24

            ShadowIcon {
                x: 1
                y: 1
                source: "qrc:/icons/eyedropper.svg"
                color: Theme.previewHudStrongOverlay
                size: 24
            }
            ShadowIcon {
                source: "qrc:/icons/eyedropper.svg"
                color: Theme.previewCompareDivider
                size: 24
            }
        }
    }
}
