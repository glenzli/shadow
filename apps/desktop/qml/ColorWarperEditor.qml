pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var controller

    readonly property int gridSide: 5
    readonly property real maximumOffset: 0.32
    readonly property var controlPoints: controller ? controller.colorWarperControlPoints : []
    readonly property bool editable: Boolean(controller && controller.active
                                              && controller.gradeNodeEnabled)
    readonly property real strength: {
        const revision = controller ? controller.parameterRevision : -1
        return revision >= 0 ? controller.parameterValue("color_warper_strength") : 1
    }

    property int selectedPoint: -1
    property bool gestureActive: false
    property int gesturePoint: -1

    implicitWidth: 320
    implicitHeight: 294

    function clamp(value, lower, upper) {
        return Math.max(lower, Math.min(upper, value))
    }

    function point(index) {
        return index >= 0 && index < controlPoints.length ? controlPoints[index] : null
    }

    function sourceX(column) {
        return warperFrame.leftInset + warperFrame.meshPadding + column / (gridSide - 1)
            * warperFrame.meshWidth
    }

    function sourceY(row) {
        return warperFrame.topInset + warperFrame.meshPadding + row / (gridSide - 1)
            * warperFrame.meshHeight
    }

    function pointX(index) {
        const value = point(index)
        if (!value)
            return 0
        return sourceX(Number(value.column))
            + Number(value.aOffset) / maximumOffset * warperFrame.meshPadding
    }

    function pointY(index) {
        const value = point(index)
        if (!value)
            return 0
        return sourceY(Number(value.row))
            - Number(value.bOffset) / maximumOffset * warperFrame.meshPadding
    }

    function nearestPoint(x, y) {
        let closest = -1
        let closestDistance = 20 * 20
        for (let index = 0; index < controlPoints.length; ++index) {
            const dx = x - pointX(index)
            const dy = y - pointY(index)
            const distance = dx * dx + dy * dy
            if (distance <= closestDistance) {
                closest = index
                closestDistance = distance
            }
        }
        return closest
    }

    function beginPointGesture(index) {
        if (!editable || index < 0)
            return
        if (gestureActive)
            finishPointGesture()
        selectedPoint = index
        gestureActive = true
        gesturePoint = index
        controller.beginParameterEdit("color_warper/point/" + index)
    }

    function movePointGesture(x, y) {
        if (!gestureActive || gesturePoint < 0)
            return
        const value = point(gesturePoint)
        if (!value)
            return
        const a = clamp((x - sourceX(Number(value.column))) / warperFrame.meshPadding
                        * maximumOffset, -maximumOffset, maximumOffset)
        const b = clamp((sourceY(Number(value.row)) - y) / warperFrame.meshPadding
                        * maximumOffset, -maximumOffset, maximumOffset)
        controller.setColorWarperControlPoint(gesturePoint, a, b)
    }

    function finishPointGesture() {
        if (!gestureActive)
            return
        const index = gesturePoint
        gestureActive = false
        gesturePoint = -1
        controller.endParameterEdit("color_warper/point/" + index)
    }

    function resetMesh() {
        finishPointGesture()
        selectedPoint = -1
        controller.resetColorWarper()
    }

    onControlPointsChanged: warperCanvas.requestPaint()
    onSelectedPointChanged: warperCanvas.requestPaint()
    onEditableChanged: {
        if (!editable)
            finishPointGesture()
    }

    Connections {
        target: root.controller

        function onSelectedGradeNodeChanged() {
            root.finishPointGesture()
            root.selectedPoint = -1
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 7

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(192, Math.min(228, root.width - 4))

            Rectangle {
                id: warperFrame

                readonly property real leftInset: 20
                readonly property real rightInset: 20
                readonly property real topInset: 20
                readonly property real bottomInset: 20
                readonly property real contentWidth: Math.max(1, width - leftInset - rightInset)
                readonly property real contentHeight: Math.max(1, height - topInset - bottomInset)
                // Reserve displacement room around the authored 5×5 anchors
                // so an edge point remains visible and can always be dragged
                // back from either extreme.
                readonly property real meshPadding: Math.max(12,
                    Math.min(contentWidth, contentHeight) * 0.12)
                readonly property real meshWidth: Math.max(1, contentWidth - meshPadding * 2)
                readonly property real meshHeight: Math.max(1, contentHeight - meshPadding * 2)

                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width, parent.height)
                height: width
                radius: 5
                color: Theme.chrome
                border.color: Theme.borderStrong
                clip: true

                Canvas {
                    id: warperCanvas

                    anchors.fill: parent

                    onPaint: {
                        const context = getContext("2d")
                        context.reset()
                        context.clearRect(0, 0, width, height)
                        if (root.controlPoints.length !== root.gridSide * root.gridSide)
                            return

                        const horizontalField = context.createLinearGradient(
                            warperFrame.leftInset, 0,
                            width - warperFrame.rightInset, 0)
                        horizontalField.addColorStop(0, "#58a976")
                        horizontalField.addColorStop(0.5, Theme.chrome)
                        horizontalField.addColorStop(1, "#d96969")
                        context.fillStyle = horizontalField
                        context.fillRect(0, 0, width, height)

                        context.globalAlpha = 0.44
                        const verticalField = context.createLinearGradient(
                            0, warperFrame.topInset, 0,
                            height - warperFrame.bottomInset)
                        verticalField.addColorStop(0, "#e6c45c")
                        verticalField.addColorStop(0.5, "transparent")
                        verticalField.addColorStop(1, "#638bd4")
                        context.fillStyle = verticalField
                        context.fillRect(0, 0, width, height)
                        context.globalAlpha = 1

                        context.lineWidth = 1
                        context.strokeStyle = Theme.curveGrid
                        for (let index = 0; index < root.gridSide; ++index) {
                            context.beginPath()
                            context.moveTo(root.sourceX(index), warperFrame.topInset)
                            context.lineTo(root.sourceX(index), height - warperFrame.bottomInset)
                            context.stroke()
                            context.beginPath()
                            context.moveTo(warperFrame.leftInset, root.sourceY(index))
                            context.lineTo(width - warperFrame.rightInset, root.sourceY(index))
                            context.stroke()
                        }

                        context.lineWidth = 1.5
                        context.lineJoin = "round"
                        context.strokeStyle = Theme.accent
                        for (let row = 0; row < root.gridSide; ++row) {
                            context.beginPath()
                            for (let column = 0; column < root.gridSide; ++column) {
                                const index = row * root.gridSide + column
                                if (column === 0)
                                    context.moveTo(root.pointX(index), root.pointY(index))
                                else
                                    context.lineTo(root.pointX(index), root.pointY(index))
                            }
                            context.stroke()
                        }
                        for (let column = 0; column < root.gridSide; ++column) {
                            context.beginPath()
                            for (let row = 0; row < root.gridSide; ++row) {
                                const index = row * root.gridSide + column
                                if (row === 0)
                                    context.moveTo(root.pointX(index), root.pointY(index))
                                else
                                    context.lineTo(root.pointX(index), root.pointY(index))
                            }
                            context.stroke()
                        }

                        for (let index = 0; index < root.controlPoints.length; ++index) {
                            const selected = index === root.selectedPoint
                            context.beginPath()
                            context.arc(root.pointX(index), root.pointY(index), selected ? 5.5 : 4,
                                        0, Math.PI * 2)
                            context.fillStyle = selected ? Theme.accent : Theme.panelRaised
                            context.fill()
                            context.lineWidth = selected ? 2 : 1
                            context.strokeStyle = Theme.accentHandleBorder
                            context.stroke()
                        }
                    }

                    Component.onCompleted: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                }

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 5
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Green")
                    color: Theme.textMuted
                    font.pixelSize: 8
                    rotation: -90
                    transformOrigin: Item.Center
                }

                Label {
                    anchors.right: parent.right
                    anchors.rightMargin: 5
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Red")
                    color: Theme.textMuted
                    font.pixelSize: 8
                    rotation: 90
                    transformOrigin: Item.Center
                }

                Label {
                    anchors.top: parent.top
                    anchors.topMargin: 3
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Yellow")
                    color: Theme.textMuted
                    font.pixelSize: 8
                }

                Label {
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 3
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Blue")
                    color: Theme.textMuted
                    font.pixelSize: 8
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: root.editable
                    hoverEnabled: true
                    cursorShape: containsMouse ? Qt.CrossCursor : Qt.ArrowCursor
                    onPressed: mouse => {
                        const index = root.nearestPoint(mouse.x, mouse.y)
                        if (index < 0)
                            return
                        root.beginPointGesture(index)
                        root.movePointGesture(mouse.x, mouse.y)
                    }
                    onPositionChanged: mouse => {
                        if (pressed)
                            root.movePointGesture(mouse.x, mouse.y)
                    }
                    onReleased: root.finishPointGesture()
                    onCanceled: root.finishPointGesture()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14

            Item { Layout.fillWidth: true }

            ShadowIconButton {
                buttonSize: 26
                iconSize: 16
                source: "qrc:/icons/redo.svg"
                variant: ShadowIconButton.Ghost
                toolTipText: qsTr("Reset color map")
                accessibleName: toolTipText
                enabled: root.editable
                onClicked: root.resetMesh()
            }
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Strength")
            from: 0.0
            to: 1.0
            neutralValue: 1.0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: root.strength
            onGestureStarted: root.controller.beginParameterEdit("color_warper_strength")
            onEdited: value => root.controller.setParameterValue("color_warper_strength", value)
            onGestureFinished: root.controller.endParameterEdit("color_warper_strength")
        }
    }
}
