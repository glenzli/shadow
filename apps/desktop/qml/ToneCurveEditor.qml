pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var controller

    property color panelColor: "#181c21"
    property color plotColor: "#101317"
    property color borderColor: "#343b43"
    property color gridColor: "#293038"
    property color textColor: "#edf0f2"
    property color mutedTextColor: "#8b949e"
    property color accentColor: "#d8b36a"

    readonly property bool hasCurve: Boolean(controller && controller.hasToneCurve)
    readonly property bool curveEditable: Boolean(controller && controller.toneCurveEditable)
    readonly property var curveModel: controller ? controller.toneCurvePoints : null

    property int selectedPoint: -1
    property bool selectedPointDeletable: false
    property real selectedPointX: 0.0
    property real selectedPointY: 0.0
    property bool gestureActive: false
    property int gesturePoint: -1

    readonly property real plotInset: 16
    readonly property real authoredPointGap: 1.0 / 4096.0

    implicitWidth: 320
    implicitHeight: 430
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

    Keys.onPressed: event => {
        if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
                && selectedPointDeletable && curveEditable) {
            removeSelectedPoint()
            event.accepted = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: "Point Curve"
                    color: root.textColor
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }

                Label {
                    text: root.hasCurve ? "Shape light and contrast directly" : "Neutral tones"
                    color: root.mutedTextColor
                    font.pixelSize: 9
                }
            }

            Rectangle {
                Layout.preferredWidth: curveStateLabel.implicitWidth + 16
                Layout.preferredHeight: 22
                radius: 11
                color: root.hasCurve ? "#30291d" : "#20252b"
                border.color: root.hasCurve ? "#5d4b2d" : root.borderColor

                Label {
                    id: curveStateLabel
                    anchors.centerIn: parent
                    text: root.hasCurve ? "ACTIVE" : "NEUTRAL"
                    color: root.hasCurve ? root.accentColor : root.mutedTextColor
                    font.pixelSize: 8
                    font.weight: Font.Bold
                    font.letterSpacing: 0.8
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(220, Math.min(350, root.width))

            Rectangle {
                id: curveFrame

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
                        context.strokeStyle = "#59636e"
                        context.beginPath()
                        context.moveTo(left, bottom)
                        context.lineTo(right, top)
                        context.stroke()

                        if (!root.hasCurve || pointRepeater.count < 2)
                            return

                        let started = false
                        context.lineWidth = 2.25
                        context.lineCap = "round"
                        context.lineJoin = "round"
                        context.strokeStyle = root.accentColor
                        context.beginPath()
                        for (let index = 0; index < pointRepeater.count; ++index) {
                            const point = root.pointItem(index)
                            if (!point)
                                continue
                            const x = root.plotX(root.pointXValue(index))
                            const y = root.plotY(root.pointYValue(index))
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

                    onDoubleClicked: mouse => {
                        const position = mapToItem(curveFrame, mouse.x, mouse.y)
                        root.addPointAt(position.x, position.y)
                    }
                }

                Column {
                    anchors.centerIn: parent
                    width: Math.max(120, parent.width - 64)
                    spacing: 5
                    visible: !root.hasCurve
                    z: 2

                    Label {
                        width: parent.width
                        text: "Neutral curve"
                        color: root.textColor
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Label {
                        width: parent.width
                        text: root.curveEditable
                            ? "Double-click anywhere to add your first point"
                            : "This curve is currently view-only"
                        color: root.mutedTextColor
                        font.pixelSize: 9
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
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
                                ? root.accentColor : root.plotColor
                            border.width: 2
                            border.color: root.accentColor
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
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: root.selectedPoint >= 0
                    ? "Selected · " + Math.round(root.selectedPointX * 100)
                        + "% → " + Math.round(root.selectedPointY * 100) + "%"
                    : (root.hasCurve ? "Double-click to add · drag to shape" : "No adjustment applied")
                color: root.mutedTextColor
                font.pixelSize: 9
                elide: Text.ElideRight
            }

            Button {
                id: removePointButton

                text: "REMOVE POINT"
                flat: true
                enabled: root.curveEditable && root.selectedPointDeletable
                onClicked: root.removeSelectedPoint()

                background: Rectangle {
                    radius: 4
                    color: removePointButton.down ? "#2b3036" : "transparent"
                    border.color: removePointButton.enabled ? root.borderColor : "#24292f"
                }

                contentItem: Label {
                    text: removePointButton.text
                    color: removePointButton.enabled ? root.textColor : "#59616a"
                    font.pixelSize: 8
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Button {
                id: resetCurveButton

                text: "RESET CURVE"
                flat: true
                enabled: root.hasCurve
                onClicked: root.resetCurve()

                background: Rectangle {
                    radius: 4
                    color: resetCurveButton.down ? "#2b3036" : "transparent"
                    border.color: resetCurveButton.enabled ? root.borderColor : "#24292f"
                }

                contentItem: Label {
                    text: resetCurveButton.text
                    color: resetCurveButton.enabled ? root.textColor : "#59616a"
                    font.pixelSize: 8
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !root.curveEditable
            text: "This curve is preserved exactly. Reset Curve is still available."
            color: "#a99268"
            font.pixelSize: 9
            wrapMode: Text.WordWrap
        }
    }
}
