pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property string label
    property real hue: 0
    property real saturation: 0
    property real luminance: 0
    property bool wheelGestureActive: false
    property bool luminanceGestureActive: false

    signal wheelGestureStarted()
    signal wheelEdited(real hue, real saturation)
    signal wheelGestureFinished()
    signal luminanceGestureStarted()
    signal luminanceEdited(real value)
    signal luminanceGestureFinished()

    implicitWidth: 112
    implicitHeight: 154

    function setWheelFromPosition(x, y) {
        const center = wheel.width / 2
        const radius = Math.max(1, center - 7)
        const dx = x - center
        const dy = y - center
        const distance = Math.sqrt(dx * dx + dy * dy)
        const nextHue = (Math.atan2(dy, dx) * 180 / Math.PI + 360) % 360
        const nextSaturation = Math.max(0, Math.min(1, distance / radius))
        wheelEdited(nextHue, nextSaturation)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        Label {
            Layout.fillWidth: true
            text: root.label
            color: Theme.textSecondary
            font.pixelSize: 10
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
        }

        Item {
            id: wheel
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Math.min(108, root.width)
            Layout.preferredHeight: width

            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                radius: width / 2
                color: Theme.panelInset
                border.width: 1
                border.color: Theme.borderStrong
            }

            Canvas {
                id: wheelCanvas
                anchors.fill: parent
                anchors.margins: 4

                onPaint: {
                    const context = getContext("2d")
                    context.clearRect(0, 0, width, height)
                    const centerX = width / 2
                    const centerY = height / 2
                    const radius = Math.max(1, Math.min(centerX, centerY) - 1)
                    const angularSteps = 96
                    const radialSteps = 20
                    const fullTurn = Math.PI * 2
                    const angularOverlap = 0.006

                    // Qt's Canvas ImageData path is backend-dependent and was
                    // producing a transparent wheel on some macOS renderers.
                    // Small vector cells are static, reliable and still cheap
                    // at this control's roughly 100-pixel size.
                    for (let radial = 0; radial < radialSteps; ++radial) {
                        const innerRadius = radius * radial / radialSteps
                        const outerRadius = radius * (radial + 1) / radialSteps + 0.35
                        const saturation = (radial + 0.5) / radialSteps
                        for (let angular = 0; angular < angularSteps; ++angular) {
                            const start = fullTurn * angular / angularSteps - angularOverlap
                            const end = fullTurn * (angular + 1) / angularSteps + angularOverlap
                            const hue = (angular + 0.5) / angularSteps
                            context.beginPath()
                            if (innerRadius < 0.01) {
                                context.moveTo(centerX, centerY)
                            } else {
                                context.moveTo(centerX + Math.cos(start) * innerRadius,
                                               centerY + Math.sin(start) * innerRadius)
                            }
                            context.lineTo(centerX + Math.cos(start) * outerRadius,
                                           centerY + Math.sin(start) * outerRadius)
                            context.arc(centerX, centerY, outerRadius, start, end, false)
                            if (innerRadius >= 0.01) {
                                context.lineTo(centerX + Math.cos(end) * innerRadius,
                                               centerY + Math.sin(end) * innerRadius)
                                context.arc(centerX, centerY, innerRadius, end, start, true)
                            }
                            context.closePath()
                            context.fillStyle = Qt.hsva(hue, saturation, 0.95, 1)
                            context.fill()
                        }
                    }
                }

                Component.onCompleted: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
            }

            Rectangle {
                readonly property real radiusLimit: Math.max(1, wheel.width / 2 - 7)
                readonly property real angle: root.hue * Math.PI / 180
                x: wheel.width / 2 + Math.cos(angle) * root.saturation * radiusLimit
                    - width / 2
                y: wheel.height / 2 + Math.sin(angle) * root.saturation * radiusLimit
                    - height / 2
                width: 12
                height: 12
                radius: 6
                color: Qt.hsva(((root.hue % 360) + 360) % 360 / 360,
                    root.saturation, 0.96, 1)
                border.width: 2
                border.color: Theme.effectiveDark ? "#f5f7f8" : "#ffffff"

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    radius: width / 2
                    color: Theme.transparent
                    border.width: 1
                    border.color: Theme.accentHandleBorder
                }
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.CrossCursor
                onPressed: mouse => {
                    root.wheelGestureActive = true
                    root.wheelGestureStarted()
                    root.setWheelFromPosition(mouse.x, mouse.y)
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        root.setWheelFromPosition(mouse.x, mouse.y)
                }
                onReleased: {
                    if (!root.wheelGestureActive)
                        return
                    root.wheelGestureActive = false
                    root.wheelGestureFinished()
                }
                onCanceled: {
                    if (!root.wheelGestureActive)
                        return
                    root.wheelGestureActive = false
                    root.wheelGestureFinished()
                }
                onDoubleClicked: {
                    root.wheelEdited(0, 0)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 3

            Label {
                text: qsTr("L")
                color: Theme.textMuted
                font.pixelSize: 9
            }

            ShadowInlineSlider {
                id: luminanceSlider
                Layout.fillWidth: true
                from: -1
                to: 1
                neutralValue: 0
                stepSize: 0.01
                value: root.luminance
                onPressedChanged: {
                    if (pressed && !root.luminanceGestureActive) {
                        root.luminanceGestureActive = true
                        root.luminanceGestureStarted()
                    } else if (!pressed && root.luminanceGestureActive) {
                        root.luminanceGestureActive = false
                        root.luminanceGestureFinished()
                    }
                }
                onMoved: root.luminanceEdited(value)
                onResetRequested: value => {
                    const ownsGesture = !root.luminanceGestureActive
                    if (ownsGesture)
                        root.luminanceGestureStarted()
                    root.luminanceEdited(value)
                    if (ownsGesture)
                        root.luminanceGestureFinished()
                }
            }

            Label {
                Layout.preferredWidth: 28
                text: Math.round(root.luminance * 100)
                color: Theme.textMuted
                font.pixelSize: 9
                horizontalAlignment: Text.AlignRight
            }
        }
    }
}
