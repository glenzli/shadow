pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: field

    required property string label
    property alias value: slider.value
    property alias from: slider.from
    property alias to: slider.to
    property alias stepSize: slider.stepSize
    property real neutralValue: from
    property bool fillFromMinimum: false
    property int decimals: 2
    property real displayMultiplier: 1.0
    property string suffix: ""
    property color accent: Theme.accent
    property bool semanticTrack: false
    property color trackStartColor: Theme.track
    property color trackMiddleColor: Theme.track
    property color trackEndColor: Theme.track
    property string startLabel: ""
    property string endLabel: ""
    property int endpointLabelWidth: 38
    property color textPrimary: Theme.textPrimary
    property color textMuted: Theme.textMuted
    property string toolTipText: ""
    // Keep one stable label column across languages. Common photographic
    // terms fit directly; exceptional labels elide and expose their full text
    // on hover instead of stealing width from the adjustment track.
    property int labelWidth: Math.max(56, Math.min(72, Math.round(width * 0.24)))
    property int valueWidth: 54
    property bool gestureActive: false
    property bool valueEditing: false
    readonly property Item dismissalSurface: field.Window.window
        ? field.Window.window.contentItem : field
    readonly property Item scrollingDismissalSurface:
        field.nearestFlickableAncestor()
    readonly property bool hasEndpointLabels:
        startLabel.length > 0 || endLabel.length > 0

    readonly property string formattedValue: qsTr("%1%2")
        .arg(Number(slider.value * displayMultiplier).toLocaleString(
            Qt.locale(), "f", decimals))
        .arg(suffix)
    readonly property string editableValue: Number(
        slider.value * displayMultiplier).toLocaleString(
            Qt.locale(), "f", decimals)

    signal edited(real value)
    signal gestureStarted()
    signal gestureFinished()

    implicitHeight: 24
    activeFocusOnTab: false

    function beginGesture() {
        if (gestureActive)
            return
        gestureActive = true
        gestureStarted()
    }

    function finishGesture() {
        keyboardSettle.stop()
        if (!gestureActive)
            return
        gestureActive = false
        gestureFinished()
    }

    function resetToNeutral() {
        const boundedNeutral = Math.max(from, Math.min(to, neutralValue))
        if (!enabled || Math.abs(value - boundedNeutral) < 0.0000001)
            return
        finishGesture()
        beginGesture()
        // Keep the presentation state synchronous with the authored edit.
        // The controller may publish pixels before its model binding returns.
        slider.value = boundedNeutral
        edited(boundedNeutral)
        finishGesture()
    }

    function beginValueEdit() {
        if (!enabled || valueEditing)
            return
        finishGesture()
        valueEditing = true
        valueInput.text = editableValue
        valueInput.forceActiveFocus(Qt.MouseFocusReason)
        valueInput.selectAll()
    }

    function cancelValueEdit() {
        if (!valueEditing)
            return
        valueEditing = false
        slider.forceActiveFocus(Qt.MouseFocusReason)
    }

    function commitValueEdit(restoreSliderFocus) {
        if (!valueEditing)
            return true

        const parsedDisplayValue = Number.fromLocaleString(
            Qt.locale(), valueInput.text.trim())
        if (!Number.isFinite(parsedDisplayValue)
                || !Number.isFinite(displayMultiplier)
                || Math.abs(displayMultiplier) < 0.0000001) {
            valueInput.text = editableValue
            if (restoreSliderFocus === true) {
                valueInput.selectAll()
            } else {
                // Clicking away cancels invalid text instead of leaving a
                // stranded, unfocused editor visible in the inspector.
                valueEditing = false
            }
            return false
        }

        let nextValue = parsedDisplayValue / displayMultiplier
        nextValue = Math.max(from, Math.min(to, nextValue))
        if (stepSize > 0) {
            nextValue = from + Math.round(
                (nextValue - from) / stepSize) * stepSize
            nextValue = Math.max(from, Math.min(to, nextValue))
        }

        valueEditing = false
        if (restoreSliderFocus === true)
            slider.forceActiveFocus(Qt.MouseFocusReason)
        if (Math.abs(nextValue - value) < 0.0000001)
            return true

        beginGesture()
        slider.value = nextValue
        edited(nextValue)
        finishGesture()
        return true
    }

    function nearestFlickableAncestor() {
        let candidate = field.parent
        while (candidate) {
            if (candidate instanceof Flickable)
                return candidate
            candidate = candidate.parent
        }
        return null
    }

    function dismissValueEditFrom(surface, eventPoint) {
        const local = valueInput.mapFromItem(
            surface, eventPoint.position.x, eventPoint.position.y)
        if (local.x < 0 || local.y < 0
                || local.x > valueInput.width
                || local.y > valueInput.height)
            field.commitValueEdit(false)
    }

    // Own dismissal beside the editor itself and observe the whole window.
    // The passive handler does not steal the click from the canvas or the
    // next control; it only commits this editor when that click lands outside.
    TapHandler {
        parent: field.dismissalSurface
        target: null
        enabled: field.valueEditing && field.visible && field.enabled
        acceptedButtons: Qt.LeftButton
        // Precision sliders live inside a ScrollView.  Its Flickable may take
        // the drag grab even for a short blank-space press, which cancels a
        // DragThreshold observer before it can dismiss this editor.
        // ReleaseWithinBounds remains an observation of the completed click:
        // it neither competes with scrolling nor consumes the target control.
        gesturePolicy: TapHandler.ReleaseWithinBounds
        grabPermissions: PointerHandler.TakeOverForbidden
        onTapped: (eventPoint, button) =>
            field.dismissValueEditFrom(parent, eventPoint)
    }

    // A ScrollView's internal Flickable is allowed to filter events before a
    // window-level pointer handler sees them.  Observe that exact local input
    // surface as well, while keeping the handler passive so blank clicks,
    // scrolling, buttons, and the next slider retain their own behavior.
    TapHandler {
        parent: field.scrollingDismissalSurface
            ? field.scrollingDismissalSurface : field
        target: null
        enabled: field.valueEditing && field.visible && field.enabled
            && field.scrollingDismissalSurface !== null
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        grabPermissions: PointerHandler.TakeOverForbidden
        onTapped: (eventPoint, button) =>
            field.dismissValueEditFrom(parent, eventPoint)
    }

    onEnabledChanged: {
        if (!enabled) {
            valueEditing = false
            finishGesture()
        }
    }

    Timer {
        id: keyboardSettle
        interval: 240
        repeat: false
        onTriggered: field.finishGesture()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 4

        Label {
            id: fieldLabel

            Layout.preferredWidth: visible ? field.labelWidth : 0
            Layout.minimumWidth: 0
            visible: !field.hasEndpointLabels
            text: field.label
            color: field.enabled ? field.textPrimary : Theme.textDisabled
            font.pixelSize: 10
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter

            HoverHandler { id: labelHover }
            ToolTip.visible: labelHover.hovered
                && (fieldLabel.truncated || field.toolTipText.length > 0)
            ToolTip.delay: 500
            ToolTip.text: field.toolTipText.length > 0 ? field.toolTipText : field.label
        }

        Item {
            visible: field.hasEndpointLabels
            Layout.preferredWidth: field.valueWidth
            Layout.minimumWidth: field.valueWidth
        }

        Label {
            visible: field.startLabel.length > 0
            Layout.preferredWidth: field.endpointLabelWidth
            Layout.minimumWidth: field.endpointLabelWidth
            text: field.startLabel
            color: field.enabled ? field.trackStartColor : Theme.textDisabled
            font.pixelSize: 9
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
        }

        ShadowInlineSlider {
            id: slider

            Layout.fillWidth: true
            Layout.minimumWidth: 64
            neutralValue: field.neutralValue
            fillFromMinimum: field.fillFromMinimum
            accent: field.accent
            semanticTrack: field.semanticTrack
            trackStartColor: field.trackStartColor
            trackMiddleColor: field.trackMiddleColor
            trackEndColor: field.trackEndColor
            snapMode: Slider.SnapAlways
            enabled: field.enabled
            adjustmentFocusTarget: true
            Accessible.name: field.label
            Accessible.description: field.formattedValue

            onMoved: {
                if (!pressed && !handleDragActive) {
                    field.beginGesture()
                    keyboardSettle.restart()
                }
                field.edited(value)
            }
            onPressedChanged: {
                if (pressed) {
                    keyboardSettle.stop()
                    field.beginGesture()
                } else if (field.gestureActive) {
                    field.finishGesture()
                }
            }
            onHandleDragStarted: {
                keyboardSettle.stop()
                field.beginGesture()
            }
            onHandleDragFinished: field.finishGesture()
            onFocusTraversalStarted: field.finishGesture()
            onResetRequested: field.resetToNeutral()
        }

        Label {
            visible: field.endLabel.length > 0
            Layout.preferredWidth: field.endpointLabelWidth
            Layout.minimumWidth: field.endpointLabelWidth
            text: field.endLabel
            color: field.enabled ? field.trackEndColor : Theme.textDisabled
            font.pixelSize: 9
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
        }

        Item {
            Layout.preferredWidth: field.valueWidth
            Layout.minimumWidth: field.valueWidth
            Layout.fillHeight: true

            Label {
                id: valueLabel
                objectName: "shadowSliderValueLabel"
                anchors.fill: parent
                visible: !field.valueEditing
                text: field.formattedValue
                color: field.enabled ? field.textMuted : Theme.textDisabled
                font.family: "Menlo"
                font.pixelSize: 9
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideLeft

                TapHandler {
                    enabled: field.enabled
                    acceptedButtons: Qt.LeftButton
                    cursorShape: Qt.IBeamCursor
                    onTapped: field.beginValueEdit()
                }
            }

            TextField {
                id: valueInput
                objectName: "shadowSliderValueEditor"
                anchors.fill: parent
                visible: field.valueEditing
                enabled: field.enabled
                activeFocusOnTab: false
                selectByMouse: true
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                color: field.textPrimary
                selectionColor: field.accent
                selectedTextColor: Theme.selectionForeground
                font.family: "Menlo"
                font.pixelSize: 9
                horizontalAlignment: TextInput.AlignRight
                verticalAlignment: TextInput.AlignVCenter
                leftPadding: 3
                rightPadding: 3
                topPadding: 0
                bottomPadding: 0

                background: Rectangle {
                    radius: 3
                    color: Theme.control
                    border.width: 1
                    border.color: valueInput.activeFocus
                        ? Theme.focusRing : Theme.border
                }

                onAccepted: field.commitValueEdit(true)
                onActiveFocusChanged: {
                    if (!activeFocus && field.valueEditing)
                        field.commitValueEdit(false)
                }
                Keys.onEscapePressed: event => {
                    field.cancelValueEdit()
                    event.accepted = true
                }
            }
        }
    }
}
