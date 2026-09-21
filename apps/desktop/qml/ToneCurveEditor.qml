pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var controller

    property color panelColor: Theme.panelRaised
    property color plotColor: Theme.chrome
    property color borderColor: Theme.borderStrong
    property color gridColor: Theme.curveGrid
    property color textColor: Theme.textPrimary
    property color mutedTextColor: Theme.textMuted
    property color accentColor: Theme.accent
    property color identityColor: Theme.curveIdentity

    readonly property bool hasCurve: Boolean(controller && controller.hasToneCurve)
    readonly property bool curveEditable: Boolean(controller && controller.toneCurveEditable)
    readonly property var curveModel: controller ? controller.toneCurvePoints : null
    readonly property int channel: controller && controller.toneCurveChannel !== undefined
        ? controller.toneCurveChannel : 0
    readonly property var modifiedChannels: controller && controller.toneCurveModifiedChannels !== undefined
        ? controller.toneCurveModifiedChannels : [false, false, false, false, false]
    readonly property var channelLabels: [qsTr("Lightness"), "RGB", "R", "G", "B"]
    readonly property var channelTips: [qsTr("Perceptual lightness"), qsTr("RGB master curve"),
        qsTr("Red channel"), qsTr("Green channel"), qsTr("Blue channel")]
    readonly property color currentCurveColor: channel === 2 ? Theme.curveRed
        : channel === 3 ? Theme.curveGreen : channel === 4 ? Theme.curveBlue
        : channel === 1 ? textColor : accentColor

    property int selectedPoint: -1
    property bool selectedPointDeletable: false
    property real selectedPointX: 0.0
    property real selectedPointY: 0.0
    property bool gestureActive: false
    property int gesturePoint: -1

    readonly property real plotInset: 16
    readonly property real authoredPointGap: 1.0 / 4096.0

    implicitWidth: 320
    implicitHeight: curveContents.implicitHeight
    activeFocusOnTab: true

    function clamp(value, lower, upper) {
        return Math.max(lower, Math.min(upper, value))
    }

    function plotX(value) {
        const span = Math.max(1, curveFrame.width - 2 * plotInset)
        return plotInset + clamp(Number(value), 0, 1) * span
    }

    function plotY(value) {
        const span = Math.max(1, curveFrame.height - 2 * plotInset)
        return plotInset + (1 - clamp(Number(value), 0, 1)) * span
    }

    function valueX(pixel) {
        const span = Math.max(1, curveFrame.width - 2 * plotInset)
        return clamp((pixel - plotInset) / span, 0, 1)
    }

    function valueY(pixel) {
        const span = Math.max(1, curveFrame.height - 2 * plotInset)
        return clamp(1 - (pixel - plotInset) / span, 0, 1)
    }

    function pointItem(index) {
        if (index < 0 || index >= pointRepeater.count)
            return null
        return pointRepeater.itemAt(index)
    }

    function pointXValue(index) {
        const point = pointItem(index)
        return point ? Number(point.xValue) : 0
    }

    function pointYValue(index) {
        const point = pointItem(index)
        return point ? Number(point.yValue) : 0
    }

    function constrainedX(index, candidate, movable) {
        if (!movable) {
            const current = pointItem(index)
            return current ? current.xValue : candidate
        }

        let lower = 0
        let upper = 1
        const previous = pointItem(index - 1)
        const next = pointItem(index + 1)
        if (previous)
            lower = Number(previous.xValue) + authoredPointGap
        if (next)
            upper = Number(next.xValue) - authoredPointGap
        return clamp(candidate, lower, upper)
    }

    function selectPoint(index, deletable, xValue, yValue) {
        selectedPoint = index
        selectedPointDeletable = Boolean(deletable)
        selectedPointX = Number(xValue)
        selectedPointY = Number(yValue)
        forceActiveFocus()
    }

    function clearSelection() {
        selectedPoint = -1
        selectedPointDeletable = false
        selectedPointX = 0
        selectedPointY = 0
    }

    function beginPointGesture(index) {
        if (!curveEditable)
            return
        if (gestureActive)
            finishPointGesture()
        gestureActive = true
        gesturePoint = index
        controller.beginToneCurveGesture(index)
    }

    function movePointGesture(index, pixelX, pixelY, movable) {
        if (!gestureActive || gesturePoint !== index || !curveEditable)
            return
        const x = constrainedX(index, valueX(pixelX), movable)
        const y = valueY(pixelY)
        selectedPointX = x
        selectedPointY = y
        controller.moveToneCurvePoint(index, x, y)
    }

    function finishPointGesture() {
        if (!gestureActive)
            return
        const index = gesturePoint
        gestureActive = false
        gesturePoint = -1
        controller.endToneCurveGesture(index)
    }

    function addPointAt(pixelX, pixelY) {
        if (!curveEditable)
            return
        finishPointGesture()
        clearSelection()
        controller.addToneCurvePoint(valueX(pixelX), valueY(pixelY))
    }

    function removeSelectedPoint() {
        if (!curveEditable || selectedPoint < 0 || !selectedPointDeletable)
            return
        finishPointGesture()
        const index = selectedPoint
        clearSelection()
        controller.removeToneCurvePoint(index)
    }

    function resetCurve() {
        if (!hasCurve)
            return
        finishPointGesture()
        clearSelection()
        controller.resetToneCurve()
    }

    Connections {
        target: root.controller

        function onSelectedGradeNodeChanged() {
            root.finishPointGesture()
            root.clearSelection()
        }
    }

    Connections {
        target: root.curveModel

        function onModelReset() {
            root.finishPointGesture()
            root.clearSelection()
            curveCanvas.requestPaint()
        }

        function onPointsChanged() {
            curveCanvas.requestPaint()
        }
    }

    onHasCurveChanged: {
        if (!hasCurve) {
            finishPointGesture()
            clearSelection()
        }
        curveCanvas.requestPaint()
    }

    onCurveEditableChanged: {
        if (!curveEditable)
            finishPointGesture()
    }

    onGridColorChanged: curveCanvas.requestPaint()
    onAccentColorChanged: curveCanvas.requestPaint()
    onIdentityColorChanged: curveCanvas.requestPaint()
    onCurrentCurveColorChanged: curveCanvas.requestPaint()

    Keys.onPressed: event => {
        if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
                && selectedPointDeletable && curveEditable) {
            removeSelectedPoint()
            event.accepted = true
        }
    }

    ColumnLayout {
        id: curveContents
        anchors.fill: parent
        spacing: 7

        RowLayout {
            Layout.fillWidth: true
            spacing: 2
            Repeater {
                model: 5
                delegate: ShadowTabButton {
                    required property int index
                    objectName: "toneCurveChannel" + index
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    minimumTabWidth: 0
                    compact: true
                    underlineMaximumWidth: 24
                    text: root.channelLabels[index]
                    toolTipText: root.channelTips[index]
                    active: root.channel === index
                    activeColor: index === 2 ? Theme.curveRed : index === 3 ? Theme.curveGreen
                        : index === 4 ? Theme.curveBlue : root.accentColor
                    onClicked: {
                        root.finishPointGesture()
                        root.clearSelection()
                        root.controller.toneCurveChannel = index
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 5
                        width: 3
                        height: 3
                        radius: 1.5
                        color: parent.activeColor
                        visible: root.modifiedChannels[parent.index] && !parent.active
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(190, Math.min(260, root.width - 4))

            Rectangle {
                id: curveFrame
                objectName: "toneCurveFrame"

                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width, parent.height)
                height: width
                radius: 5
                color: root.plotColor
                border.color: root.borderColor
                clip: true

                Canvas {
                    id: curveCanvas

                    anchors.fill: parent

                    onPaint: {
                        const context = getContext("2d")
                        context.reset()
                        context.clearRect(0, 0, width, height)

                        const left = root.plotInset
                        const top = root.plotInset
                        const right = width - root.plotInset
                        const bottom = height - root.plotInset

                        context.lineWidth = 1
                        context.strokeStyle = root.gridColor
                        for (let index = 0; index <= 4; ++index) {
                            const fraction = index / 4
                            const x = left + fraction * (right - left)
                            const y = top + fraction * (bottom - top)
                            context.beginPath()
                            context.moveTo(x, top)
                            context.lineTo(x, bottom)
                            context.stroke()
                            context.beginPath()
                            context.moveTo(left, y)
                            context.lineTo(right, y)
                            context.stroke()
                        }

                        context.lineWidth = 1
                        context.strokeStyle = root.identityColor
                        context.beginPath()
                        context.moveTo(left, bottom)
                        context.lineTo(right, top)
                        context.stroke()

                        if (!root.hasCurve || !root.curveModel
                                || pointRepeater.count < 2)
                            return

                        const sampleCount = Math.max(65, Math.min(
                            513, Math.ceil(right - left) + 1))
                        const samples = root.curveModel.sampledPoints(
                            sampleCount, true)
                        if (!samples || samples.length < 2)
                            return
                        let started = false
                        context.lineWidth = 2.25
                        context.lineCap = "round"
                        context.lineJoin = "round"
                        context.strokeStyle = root.currentCurveColor
                        context.beginPath()
                        for (let index = 0; index < samples.length; ++index) {
                            const sample = samples[index]
                            const x = root.plotX(Number(sample.x))
                            const y = root.plotY(Number(sample.y))
                            if (!started) {
                                context.moveTo(x, y)
                                started = true
                            } else {
                                context.lineTo(x, y)
                            }
                        }
                        if (started)
                            context.stroke()
                    }
                }

                MouseArea {
                    id: addPointArea

                    anchors.fill: parent
                    anchors.margins: root.plotInset
                    z: 1
                    enabled: root.curveEditable
                    hoverEnabled: true
                    cursorShape: enabled ? Qt.CrossCursor : Qt.ArrowCursor

                    onClicked: mouse => {
                        const position = mapToItem(curveFrame, mouse.x, mouse.y)
                        root.addPointAt(position.x, position.y)
                    }
                }

                Repeater {
                    id: pointRepeater

                    model: root.curveModel

                    onCountChanged: {
                        if (root.selectedPoint >= count)
                            root.clearSelection()
                        curveCanvas.requestPaint()
                    }

                    delegate: Item {
                        id: pointHandle

                        required property int index
                        required property real xValue
                        required property real yValue
                        required property bool endpoint
                        required property bool xMovable
                        required property bool deletable

                        width: 30
                        height: 30
                        x: root.plotX(xValue) - width / 2
                        y: root.plotY(yValue) - height / 2
                        z: 4
                        visible: root.hasCurve

                        onXValueChanged: {
                            if (root.selectedPoint === index)
                                root.selectedPointX = Number(xValue)
                            curveCanvas.requestPaint()
                        }
                        onYValueChanged: {
                            if (root.selectedPoint === index)
                                root.selectedPointY = Number(yValue)
                            curveCanvas.requestPaint()
                        }
                        onDeletableChanged: {
                            if (root.selectedPoint === index)
                                root.selectedPointDeletable = Boolean(deletable)
                        }
                        Component.onCompleted: curveCanvas.requestPaint()
                        Component.onDestruction: curveCanvas.requestPaint()

                        Rectangle {
                            anchors.centerIn: parent
                            width: root.selectedPoint === pointHandle.index ? 13 : 11
                            height: width
                            radius: width / 2
                            color: root.selectedPoint === pointHandle.index
                                ? root.currentCurveColor : root.plotColor
                            border.width: 2
                            border.color: root.currentCurveColor
                        }

                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            hoverEnabled: true
                            preventStealing: true
                            cursorShape: root.curveEditable
                                ? Qt.SizeAllCursor : Qt.ArrowCursor

                            onPressed: mouse => {
                                root.selectPoint(
                                    pointHandle.index,
                                    pointHandle.deletable,
                                    pointHandle.xValue,
                                    pointHandle.yValue
                                )
                                if (root.curveEditable)
                                    root.beginPointGesture(pointHandle.index)
                                mouse.accepted = true
                            }

                            onPositionChanged: mouse => {
                                if (!pressed || !root.curveEditable)
                                    return
                                const position = mapToItem(curveFrame, mouse.x, mouse.y)
                                root.movePointGesture(
                                    pointHandle.index,
                                    position.x,
                                    position.y,
                                    pointHandle.xMovable
                                )
                            }

                            onReleased: mouse => {
                                root.finishPointGesture()
                                mouse.accepted = true
                            }

                            onCanceled: root.finishPointGesture()
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 5

            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: root.selectedPoint >= 0
                    ? qsTr("Selected · %L1% → %L2%")
                        .arg(Math.round(root.selectedPointX * 100))
                        .arg(Math.round(root.selectedPointY * 100))
                    : (!root.curveEditable ? qsTr("Read only") : "")
                color: root.mutedTextColor
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }

            ShadowIconButton {
                id: removePointButton

                buttonSize: 26
                iconSize: 16
                source: "qrc:/icons/trash.svg"
                variant: ShadowIconButton.Ghost
                toolTipText: qsTr("Remove point")
                accessibleName: toolTipText
                enabled: root.curveEditable && root.selectedPointDeletable
                onClicked: root.removeSelectedPoint()
            }

            ShadowIconButton {
                id: resetCurveButton

                buttonSize: 26
                iconSize: 16
                source: "qrc:/icons/redo.svg"
                variant: ShadowIconButton.Ghost
                toolTipText: qsTr("Reset current channel")
                accessibleName: toolTipText
                enabled: root.hasCurve
                onClicked: root.resetCurve()
            }

        }
    }
}
