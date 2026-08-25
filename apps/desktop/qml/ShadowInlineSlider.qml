pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Slider {
    id: control
    objectName: adjustmentFocusTarget ? "shadowAdjustmentSliderInput" : ""

    property real neutralValue: from
    property bool showNeutralMarker: neutralValue > from && neutralValue < to
    // Most adjustments visualize distance from their neutral value. Amount
    // controls instead fill from the logical minimum while keeping an
    // independent reset value (for example AI strength resets to 100%).
    property bool fillFromMinimum: false
    property color accent: Theme.accent
    property bool semanticTrack: false
    property color trackStartColor: Theme.track
    property color trackMiddleColor: Theme.track
    property color trackEndColor: Theme.track
    property string toolTipText: ""
    // Only public adjustment rows opt into vertical focus traversal. Direct
    // inline sliders in toolbars and settings keep their existing key model.
    property bool adjustmentFocusTarget: false
    property bool handleDragActive: false

    readonly property real logicalNeutralPosition: to === from
        ? 0 : Math.max(0, Math.min(1, (neutralValue - from) / (to - from)))
    readonly property real neutralPosition: mirrored
        ? 1 - logicalNeutralPosition : logicalNeutralPosition
    readonly property real minimumVisualPosition: mirrored ? 1 : 0
    readonly property real fillStartPosition: fillFromMinimum
        ? Math.min(visualPosition, minimumVisualPosition)
        : Math.min(visualPosition, neutralPosition)
    readonly property real fillEndPosition: fillFromMinimum
        ? Math.max(visualPosition, minimumVisualPosition)
        : Math.max(visualPosition, neutralPosition)

    signal resetRequested(real value)
    signal focusTraversalStarted()
    signal handleDragStarted()
    signal handleDragFinished()

    implicitWidth: 112
    implicitHeight: 22
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: adjustmentFocusTarget

    function requestNeutralReset() {
        const boundedNeutral = Math.max(from, Math.min(to, neutralValue))
        if (!enabled || Math.abs(value - boundedNeutral) < 0.0000001)
            return
        resetRequested(boundedNeutral)
    }

    function moveAdjustmentFocus(forward) {
        if (!adjustmentFocusTarget)
            return false

        let candidate = control.nextItemInFocusChain(forward)
        while (candidate && candidate !== control) {
            if (candidate.objectName === "shadowAdjustmentSliderInput"
                    && candidate.enabled && candidate.visible) {
                control.focusTraversalStarted()
                candidate.forceActiveFocus(Qt.TabFocusReason)
                return true
            }
            candidate = candidate.nextItemInFocusChain(forward)
        }
        // Adjustment rows reserve vertical arrows for traversal even at a
        // boundary; falling through would make Up/Down unexpectedly edit the
        // horizontal value.
        return true
    }

    Keys.onUpPressed: event => {
        event.accepted = control.moveAdjustmentFocus(false)
    }
    Keys.onDownPressed: event => {
        event.accepted = control.moveAdjustmentFocus(true)
    }

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: control.semanticTrack ? 5 : 3
        radius: height / 2
        color: control.enabled ? Theme.track : Theme.borderDisabled
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0
                color: control.enabled && control.semanticTrack
                    ? control.trackStartColor : Theme.track
            }
            GradientStop {
                position: 0.5
                color: control.enabled && control.semanticTrack
                    ? control.trackMiddleColor : Theme.track
            }
            GradientStop {
                position: 1
                color: control.enabled && control.semanticTrack
                    ? control.trackEndColor : Theme.track
            }
        }

        Rectangle {
            visible: !control.semanticTrack
            x: control.fillStartPosition * parent.width
            width: (control.fillEndPosition - control.fillStartPosition)
                * parent.width
            height: parent.height
            radius: parent.radius
            color: control.enabled ? control.accent : Theme.textDisabled
        }

        Rectangle {
            visible: control.showNeutralMarker
            x: Math.round(control.neutralPosition * parent.width) - width / 2
            y: -2
            width: 1
            height: parent.height + 4
            color: control.enabled ? Theme.textMuted : Theme.textDisabled
        }
    }

    handle: Rectangle {
        id: handleItem
        objectName: "shadowInlineSliderHandle"
        x: control.leftPadding
            + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 12
        implicitHeight: 12
        radius: 6
        color: !control.enabled
            ? Theme.buttonDisabledSurface
            : (control.pressed || handleMouse.pressed)
                ? Theme.accentHover : Theme.panelRaised
        border.width: control.visualFocus ? 2 : 1
        border.color: !control.enabled
            ? Theme.borderDisabled
            : control.visualFocus ? Theme.focusRing : control.accent

        Behavior on color {
            ColorAnimation { duration: 80 }
        }

        // A thumb press by itself deliberately does not start an adjustment,
        // leaving Qt's double-click recognizer intact. Once movement crosses
        // the drag threshold this area emits one explicit edit gesture and
        // updates the Slider's public value/moved contract.
        MouseArea {
            id: handleMouse

            anchors.fill: parent
            anchors.margins: -5
            enabled: control.enabled
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor

            property real pressCenterOffset: 0
            property point pressPoint: Qt.point(0, 0)

            function pointInControl(mouse) {
                return mapToItem(control, mouse.x, mouse.y)
            }

            function finishDrag() {
                if (!control.handleDragActive)
                    return
                control.handleDragActive = false
                control.handleDragFinished()
            }

            onPressed: mouse => {
                const point = pointInControl(mouse)
                pressPoint = point
                pressCenterOffset = point.x
                    - (handleItem.x + handleItem.width / 2)
                control.forceActiveFocus(Qt.MouseFocusReason)
            }

            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const point = pointInControl(mouse)
                if (!control.handleDragActive) {
                    const dx = point.x - pressPoint.x
                    const dy = point.y - pressPoint.y
                    if (Math.sqrt(dx * dx + dy * dy) < 4)
                        return
                    control.handleDragActive = true
                    control.handleDragStarted()
                }

                const travel = control.availableWidth - handleItem.width
                if (travel <= 0)
                    return
                const minimumCenter = control.leftPadding + handleItem.width / 2
                const maximumCenter = minimumCenter + travel
                const desiredCenter = Math.max(minimumCenter, Math.min(
                    maximumCenter, point.x - pressCenterOffset))
                const visual = (desiredCenter - minimumCenter) / travel
                const logical = control.mirrored ? 1 - visual : visual
                let nextValue = control.from
                    + logical * (control.to - control.from)
                if (control.stepSize > 0) {
                    nextValue = control.from + Math.round(
                        (nextValue - control.from) / control.stepSize)
                        * control.stepSize
                }
                control.value = Math.max(control.from, Math.min(
                    control.to, nextValue))
                control.moved()
            }

            onReleased: finishDrag()
            onCanceled: finishDrag()
            onDoubleClicked: mouse => {
                mouse.accepted = true
                control.requestNeutralReset()
            }
        }
    }

    ToolTip {
        id: toolTip

        parent: control
        visible: control.enabled && control.hovered
            && !control.pressed && control.toolTipText.length > 0
        delay: 450
        timeout: 4000
        text: control.toolTipText
        x: Math.round((control.width - width) / 2)
        y: control.height + 6

        contentItem: Label {
            text: toolTip.text
            color: Theme.textPrimary
            font.pixelSize: Theme.fontMeta
            font.weight: Font.Medium
        }

        background: Rectangle {
            radius: Theme.compactControlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
    }
}
