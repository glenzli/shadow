pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var controller
    required property var bands

    property color panelColor: Theme.panelRaised
    property color plotColor: Theme.chrome
    property color borderColor: Theme.borderStrong
    property color gridColor: Theme.curveGrid
    property color textColor: Theme.textPrimary
    property color mutedTextColor: Theme.textMuted
    property color accentColor: Theme.accent
    property color identityColor: Theme.curveIdentity

    readonly property int parameterRevision: controller ? controller.parameterRevision : -1
    readonly property bool editable: Boolean(controller && controller.active
                                             && controller.gradeNodeEnabled)
    readonly property real horizontalInset: 18
    readonly property real verticalInset: 17
    readonly property string curveComponent: curveMode === 0 ? "hue"
        : curveMode === 1 ? "saturation" : "lightness"

    property int curveMode: 0
    property int selectedAnchor: -1
    property int gestureAnchor: -1
    property string gestureComponent: ""

    // One immutable snapshot per visible parameter revision. Canvas geometry
    // and labels must not repeatedly cross the controller boundary while painting.
    readonly property var anchorValues: {
        const revision = parameterRevision
        const values = []
        if (visible && revision >= 0 && controller && bands) {
            for (let index = 0; index < bands.length; ++index)
                values.push(Number(controller.colorMixerValue(index, curveComponent)))
        }
        return values
    }

    implicitWidth: 320
    implicitHeight: curveContents.implicitHeight

    function clamp(value, lower, upper) {
        return Math.max(lower, Math.min(upper, value))
    }

    function anchorHue(index) {
        const band = bands && index >= 0 && index < bands.length ? bands[index] : null
        return band ? Number(band.oklchHue) : 0
    }

    function anchorValue(index) {
        return index >= 0 && index < anchorValues.length ? anchorValues[index] : 0
    }

    function formattedValue(value) {
        const normalized = clamp(Number(value), -1, 1)
        const magnitude = curveComponent === "hue" ? Math.round(Math.abs(normalized) * 30)
            : curveComponent === "saturation" ? Math.round(Math.abs(normalized) * 100)
            : Math.round(Math.abs(normalized) * 15)
        const sign = normalized > 0 ? "+" : normalized < 0 ? "−" : ""
        const suffix = curveComponent === "hue" ? "°"
            : curveComponent === "saturation" ? "%" : " L"
        return sign + magnitude + suffix
    }

    function upperScaleLabel() {
        return curveComponent === "hue" ? qsTr("+30°")
            : curveComponent === "saturation" ? qsTr("+100%") : qsTr("+15 L")
    }

    function lowerScaleLabel() {
        return curveComponent === "hue" ? qsTr("−30°")
            : curveComponent === "saturation" ? qsTr("−100%") : qsTr("−15 L")
    }

    function idleInstruction() {
        return curveComponent === "hue"
            ? qsTr("Drag a color anchor vertically to shift its hue")
            : curveComponent === "saturation"
                ? qsTr("Drag a color anchor vertically to change its chroma")
                : qsTr("Drag a color anchor vertically to change its lightness")
    }

    function hueGradient(context, left, right) {
        const gradient = context.createLinearGradient(left, 0, right, 0)
        if (!bands || bands.length === 0) {
            gradient.addColorStop(0, accentColor)
            gradient.addColorStop(1, accentColor)
            return gradient
        }
        gradient.addColorStop(0, bands[0].hueLow)
        for (let index = 0; index < bands.length; ++index)
            gradient.addColorStop(clamp(anchorHue(index) / 360, 0, 1),
                                  bands[index].color)
        gradient.addColorStop(1, bands[0].hueLow)
        return gradient
    }

    function plotLeft() { return horizontalInset }
    function plotRight() { return Math.max(horizontalInset + 1, curveFrame.width - horizontalInset) }
    function plotTop() { return verticalInset }
    function plotBottom() { return Math.max(verticalInset + 1, curveFrame.height - verticalInset) }
    function plotWidth() { return plotRight() - plotLeft() }
    function plotHeight() { return plotBottom() - plotTop() }

    function xForHue(hue) {
        return plotLeft() + clamp(Number(hue), 0, 360) / 360 * plotWidth()
    }

    function yForValue(value) {
        return plotTop() + (1 - (clamp(Number(value), -1, 1) + 1) / 2) * plotHeight()
    }

    function valueForY(y) {
        return clamp(1 - 2 * (y - plotTop()) / plotHeight(), -1, 1)
    }

    function valueAtHue(hue) {
        if (!bands || bands.length === 0)
            return 0
        const value = clamp(Number(hue), 0, 360)
        const firstHue = anchorHue(0)
        const lastIndex = bands.length - 1
        const lastHue = anchorHue(lastIndex)

        let leftIndex = lastIndex
        let rightIndex = 0
        let leftHue = lastHue - 360
        let rightHue = firstHue
        let unwrappedHue = value < firstHue ? value : value - 360
        if (value >= firstHue && value <= lastHue) {
            for (let index = 0; index < lastIndex; ++index) {
                if (value >= anchorHue(index) && value <= anchorHue(index + 1)) {
                    leftIndex = index
                    rightIndex = index + 1
                    leftHue = anchorHue(index)
                    rightHue = anchorHue(index + 1)
                    unwrappedHue = value
                    break
                }
            }
        }
        if (value > lastHue) {
            leftHue = lastHue
            rightHue = firstHue + 360
            unwrappedHue = value
        }
        const span = Math.max(0.0001, rightHue - leftHue)
        const fraction = clamp((unwrappedHue - leftHue) / span, 0, 1)
        return anchorValue(leftIndex) * (1 - fraction) + anchorValue(rightIndex) * fraction
    }

    function nearestAnchor(x, y) {
        let nearest = -1
        let distanceSquared = Math.max(12, curveFrame.width * 0.045)
        distanceSquared *= distanceSquared
        for (let index = 0; index < bands.length; ++index) {
            const dx = x - xForHue(anchorHue(index))
            const dy = y - yForValue(anchorValue(index))
            const candidate = dx * dx + dy * dy
            if (candidate <= distanceSquared) {
                nearest = index
                distanceSquared = candidate
            }
        }
        return nearest
    }

    function beginGesture(index) {
        if (!editable || index < 0)
            return
        finishGesture()
        selectedAnchor = index
        gestureAnchor = index
        gestureComponent = curveComponent
        controller.beginParameterEdit("color_mixer/" + gestureComponent + "/" + index)
    }

    function moveGesture(y) {
        if (gestureAnchor < 0)
            return
        controller.setColorMixerValue(gestureAnchor, gestureComponent, valueForY(y))
    }

    function finishGesture() {
        if (gestureAnchor < 0)
            return
        const index = gestureAnchor
        const component = gestureComponent
        gestureAnchor = -1
        gestureComponent = ""
        controller.endParameterEdit("color_mixer/" + component + "/" + index)
    }

    onAnchorValuesChanged: curveCanvas.requestPaint()
    onBandsChanged: curveCanvas.requestPaint()
    onCurveComponentChanged: {
        finishGesture()
        curveCanvas.requestPaint()
    }
    onSelectedAnchorChanged: curveCanvas.requestPaint()
    onEditableChanged: {
        if (!editable)
            finishGesture()
    }

    Connections {
        target: root.controller

        function onSelectedGradeNodeChanged() {
            root.finishGesture()
            root.selectedAnchor = -1
        }
    }

    ColumnLayout {
        id: curveContents
        anchors.fill: parent
        spacing: 7

        TabBar {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            currentIndex: root.curveMode
            background: Item {}
            onCurrentIndexChanged: root.curveMode = currentIndex

            ShadowTabButton {
                text: qsTr("HUE → HUE")
                compact: true
                underlineMaximumWidth: 32
            }
            ShadowTabButton {
                text: qsTr("HUE → CHROMA")
                compact: true
                underlineMaximumWidth: 32
            }
            ShadowTabButton {
                text: qsTr("HUE → LIGHTNESS")
                compact: true
                underlineMaximumWidth: 32
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(182, Math.min(220, root.width * 0.66))

            Rectangle {
                id: curveFrame

                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width, parent.height * 1.48)
                height: parent.height
                radius: 5
                color: root.plotColor
                border.color: root.borderColor
                clip: true

                Canvas {
                    id: curveCanvas

                    anchors.fill: parent

                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    Component.onCompleted: requestPaint()
                    onPaint: {
                        const context = getContext("2d")
                        context.reset()
                        context.clearRect(0, 0, width, height)

                        const left = root.plotLeft()
                        const right = root.plotRight()
                        const top = root.plotTop()
                        const bottom = root.plotBottom()
                        const zero = root.yForValue(0)

                        const spectrum = root.hueGradient(context, left, right)
                        context.globalAlpha = 0.11
                        context.fillStyle = spectrum
                        context.fillRect(left, top, right - left, bottom - top)
                        context.globalAlpha = 0.9
                        context.fillRect(left, bottom - 4, right - left, 4)
                        context.globalAlpha = 1

                        context.lineWidth = 1
                        context.strokeStyle = root.gridColor
                        for (let index = 0; index <= 4; ++index) {
                            const x = left + index / 4 * (right - left)
                            context.beginPath()
                            context.moveTo(x, top)
                            context.lineTo(x, bottom)
                            context.stroke()
                        }
                        context.beginPath()
                        context.moveTo(left, top)
                        context.lineTo(right, top)
                        context.moveTo(left, bottom)
                        context.lineTo(right, bottom)
                        context.stroke()

                        context.lineWidth = 1
                        context.setLineDash([3, 3])
                        context.strokeStyle = root.identityColor
                        context.beginPath()
                        context.moveTo(left, zero)
                        context.lineTo(right, zero)
                        context.stroke()
                        context.setLineDash([])

                        if (!root.bands || root.bands.length === 0)
                            return

                        context.lineWidth = 2.25
                        context.lineCap = "round"
                        context.lineJoin = "round"
                        context.strokeStyle = root.accentColor
                        context.beginPath()
                        // The authored curve is piecewise linear with a wrapped
                        // red seam. Draw its exact vertices, independent of width.
                        context.moveTo(left, root.yForValue(root.valueAtHue(0)))
                        for (let index = 0; index < root.bands.length; ++index)
                            context.lineTo(root.xForHue(root.anchorHue(index)),
                                           root.yForValue(root.anchorValue(index)))
                        context.lineTo(right, root.yForValue(root.valueAtHue(360)))
                        context.stroke()

                        for (let index = 0; index < root.bands.length; ++index) {
                            const selected = index === root.selectedAnchor
                            context.beginPath()
                            context.arc(root.xForHue(root.anchorHue(index)),
                                        root.yForValue(root.anchorValue(index)),
                                        selected ? 5.5 : 4, 0, Math.PI * 2)
                            context.fillStyle = root.bands[index].color
                            context.fill()
                            context.lineWidth = selected ? 2 : 1
                            context.strokeStyle = selected ? root.textColor : root.borderColor
                            context.stroke()
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: root.editable
                    hoverEnabled: true
                    cursorShape: containsMouse ? Qt.CrossCursor : Qt.ArrowCursor
                    onPressed: mouse => {
                        const index = root.nearestAnchor(mouse.x, mouse.y)
                        if (index < 0)
                            return
                        root.beginGesture(index)
                        root.moveGesture(mouse.y)
                    }
                    onPositionChanged: mouse => {
                        if (pressed)
                            root.moveGesture(mouse.y)
                    }
                    onReleased: root.finishGesture()
                    onCanceled: root.finishGesture()
                }

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 5
                    anchors.top: parent.top
                    anchors.topMargin: 3
                    text: root.upperScaleLabel()
                    color: root.mutedTextColor
                    font.pixelSize: Theme.fontMicro
                }

                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 5
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 3
                    text: root.lowerScaleLabel()
                    color: root.mutedTextColor
                    font.pixelSize: Theme.fontMicro
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14

            Label {
                Layout.fillWidth: true
                text: root.selectedAnchor >= 0 && root.bands
                    ? qsTr("%1 · input %2° · output %3")
                        .arg(root.bands[root.selectedAnchor].name)
                        .arg(Math.round(root.anchorHue(root.selectedAnchor)))
                        .arg(root.formattedValue(root.anchorValue(root.selectedAnchor)))
                    : root.idleInstruction()
                color: root.mutedTextColor
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Label {
                text: qsTr("INPUT HUE")
                color: root.mutedTextColor
                font.pixelSize: Theme.fontMicro
                font.weight: Font.DemiBold
                font.letterSpacing: 0.5
            }
        }

    }
}
