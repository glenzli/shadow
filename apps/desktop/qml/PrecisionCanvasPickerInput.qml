pragma ComponentBehavior: Bound

import QtQuick

// Direct-manipulation owner for preview sampling and repair-stroke authoring.
// The canvas supplies one generation-verified preview surface and its painted
// content rectangle; this component owns coordinate normalization, picker
// routing, transient stroke sampling, terminal commit, and the pointer
// affordance as one interaction lifecycle.
Item {
    id: pickerInput

    required property var editor
    required property Item previewItem
    required property rect previewContentRect
    required property bool previewFrameReady
    required property string readyPreviewGeneration
    required property real displayScale
    required property real levelZeroWidth
    required property real levelZeroHeight
    required property bool interactionEnabled

    visible: interactionEnabled
    enabled: visible

    function normalizedContentPoint(sourceItem, sourceX, sourceY) {
        if (previewContentRect.width <= 0 || previewContentRect.height <= 0) {
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
        if (!previewFrameReady || readyPreviewGeneration.length === 0)
            return
        const normalized = normalizedContentPoint(
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

    PrecisionActiveStrokeCoverage {
        id: activeRetouchCoverage
        objectName: "activeRetouchCoverage"
        anchors.fill: parent
        z: 1
        visible: inputArea.retouchStrokeActive
        radiusPixels: inputArea.retouchBrushDiameter() / 2
        coverageColor: Theme.maskCoverageTint
    }

    MouseArea {
        id: inputArea
        anchors.fill: parent
        z: 2
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
        property var retouchDraftPoints: []

        function retouchBrushDiameter() {
            // The persisted repair target has an 18px level-zero radius.
            // Coverage must remain exact even when the image is fitted far
            // below 1:1. Pointer affordance is handled separately.
            return Math.max(0.5, 36 * pickerInput.displayScale)
        }

        function appendRetouchDraftPoint(mouse, force) {
            if (retouchDraftPoints.length >= 512) {
                const compacted = []
                for (let index = 0;
                     index < retouchDraftPoints.length;
                     index += 2) {
                    compacted.push(retouchDraftPoints[index])
                }
                retouchDraftPoints = compacted
            }
            const normalized = pickerInput.normalizedContentPoint(
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
            if (lastRetouchStrokeX >= 0) {
                activeRetouchCoverage.appendSegment(
                    lastRetouchStrokeX,
                    lastRetouchStrokeY,
                    mouse.x,
                    mouse.y
                )
            }
            retouchDraftPoints.push({
                "x": normalized.x,
                "y": normalized.y
            })
            lastRetouchStrokeX = mouse.x
            lastRetouchStrokeY = mouse.y
        }

        function finishRetouchGesture(mouse, canceled) {
            if (!retouchGestureActive)
                return
            if (retouchStrokeActive) {
                if (mouse !== undefined && mouse !== null)
                    appendRetouchDraftPoint(mouse, true)
                if (retouchDraftPoints.length > 0) {
                    pickerInput.editor.addRetouchStrokeFromPreview(
                        retouchDraftPoints,
                        pickerInput.readyPreviewGeneration,
                        Math.max(1, Math.round(pickerInput.levelZeroWidth)),
                        Math.max(1, Math.round(pickerInput.levelZeroHeight)))
                }
                Qt.callLater(activeRetouchCoverage.clearStroke)
            } else if (!canceled && retouchPressPoint !== null) {
                // A click remains a single legacy spot: existing recipes and
                // the precise spot workflow retain their original behavior.
                pickerInput.editor.addRetouchSpotFromPreview(
                    retouchPressPoint.x,
                    retouchPressPoint.y,
                    pickerInput.readyPreviewGeneration,
                    Math.max(1, Math.round(pickerInput.levelZeroWidth)),
                    Math.max(1, Math.round(pickerInput.levelZeroHeight)))
            }
            retouchGestureActive = false
            retouchStrokeActive = false
            retouchPressPoint = null
            retouchPressX = -1
            retouchPressY = -1
            lastRetouchStrokeX = -1
            lastRetouchStrokeY = -1
            retouchDraftPoints = []
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
                retouchStrokeActive = true
                lastRetouchStrokeX = retouchPressX
                lastRetouchStrokeY = retouchPressY
                retouchDraftPoints = [{
                    "x": retouchPressPoint.x,
                    "y": retouchPressPoint.y
                }]
                activeRetouchCoverage.beginStroke(
                    retouchPressX, retouchPressY)
            }
            appendRetouchDraftPoint(mouse, false)
        }

        onPressed: mouse => {
            pointerX = mouse.x
            pointerY = mouse.y
            if (!pickerInput.editor.retouchPickerActive)
                return
            const normalized = pickerInput.normalizedContentPoint(
                inputArea, mouse.x, mouse.y)
            if (normalized === null)
                return
            // Option on macOS and Alt on Windows/Linux establish the durable
            // source anchor for the following repair gestures. Sampling is a
            // tool-session action, so it must never create an empty target.
            if ((mouse.modifiers & Qt.AltModifier) !== 0) {
                pickerInput.editor.setRetouchSourceFromPreview(
                    normalized.x, normalized.y)
                retouchGestureActive = false
                mouse.accepted = true
                return
            }
            retouchGestureActive = true
            retouchStrokeActive = false
            retouchPressX = mouse.x
            retouchPressY = mouse.y
            retouchPressPoint = normalized
            retouchDraftPoints = []
        }
        onReleased: mouse => finishRetouchGesture(mouse, false)
        onCanceled: finishRetouchGesture(null, true)
        onClicked: mouse => {
            if (!pickerInput.editor.retouchPickerActive) {
                pickerInput.pickPreviewColor(
                    inputArea, mouse.x, mouse.y)
            }
        }
    }

    Item {
        z: 3
        visible: inputArea.containsMouse
        x: inputArea.pointerX
        y: inputArea.pointerY

        Rectangle {
            visible: pickerInput.editor.retouchPickerActive
            anchors.centerIn: parent
            width: inputArea.retouchBrushDiameter()
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

            Rectangle {
                anchors.centerIn: parent
                width: 4
                height: 4
                radius: 2
                color: Theme.previewCompareDivider
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
