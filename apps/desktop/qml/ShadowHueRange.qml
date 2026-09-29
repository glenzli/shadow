pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property string label
    property real lowerValue: 0
    property real upperValue: 360
    property real minimumGap: 10
    property color accent: Theme.accent
    property int labelWidth: Math.max(48, Math.min(58, Math.round(width * 0.19)))
    property int valueWidth: 54
    property int activeHandle: -1
    property bool gestureActive: false

    signal gestureStarted()
    signal edited(real lowerValue, real upperValue)
    signal gestureFinished()

    implicitHeight: 42

    function positionFor(value) {
        return rangeTrack.handleInset + Math.max(0, Math.min(1, value / 360))
            * Math.max(1, rangeTrack.width - rangeTrack.handleInset * 2)
    }

    function valueFor(position) {
        return Math.max(0, Math.min(360,
            (position - rangeTrack.handleInset)
                / Math.max(1, rangeTrack.width - rangeTrack.handleInset * 2) * 360))
    }

    function updateHandle(position) {
        const candidate = valueFor(position)
        if (activeHandle === 0)
            edited(Math.min(candidate, upperValue - minimumGap), upperValue)
        else if (activeHandle === 1)
            edited(lowerValue, Math.max(candidate, lowerValue + minimumGap))
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 3

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.preferredWidth: root.labelWidth
                Layout.minimumWidth: 0
                text: root.label
                color: Theme.textPrimary
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignRight
            }
            Item { Layout.fillWidth: true }
            Label {
                Layout.preferredWidth: root.valueWidth
                Layout.minimumWidth: root.valueWidth
                text: qsTr("%1°–%2°").arg(Math.round(root.lowerValue))
                    .arg(Math.round(root.upperValue))
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                horizontalAlignment: Text.AlignRight
            }
        }

        Item {
            id: rangeTrack
            Layout.fillWidth: true
            Layout.leftMargin: root.labelWidth + 4
            Layout.rightMargin: root.valueWidth + 4
            Layout.preferredHeight: 18

            readonly property real handleInset: 5

            Rectangle {
                id: hueTrack
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: rangeTrack.handleInset
                anchors.rightMargin: rangeTrack.handleInset
                anchors.verticalCenter: parent.verticalCenter
                height: 7
                radius: 4
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "#ff4d4d" }
                    GradientStop { position: 0.1667; color: "#ffd84d" }
                    GradientStop { position: 0.3333; color: "#5fd35f" }
                    GradientStop { position: 0.5; color: "#4ddddd" }
                    GradientStop { position: 0.6667; color: "#5680e9" }
                    GradientStop { position: 0.8333; color: "#d05ce3" }
                    GradientStop { position: 1.0; color: "#ff4d4d" }
                }
                opacity: 0.55

                Rectangle {
                    x: root.positionFor(root.lowerValue) - hueTrack.x
                    width: Math.max(2, root.positionFor(root.upperValue)
                        - root.positionFor(root.lowerValue))
                    height: parent.height
                    radius: parent.radius
                    color: root.accent
                    opacity: 0.46
                }
            }

            Repeater {
                model: 2
                delegate: Rectangle {
                    required property int index
                    x: root.positionFor(index === 0 ? root.lowerValue : root.upperValue)
                        - width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    radius: 6
                    color: Theme.panelRaised
                    border.width: 1
                    border.color: root.activeHandle === index
                        ? Theme.focusRing : root.accent
                }
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SizeHorCursor
                onPressed: mouse => {
                    const lowerDistance = Math.abs(mouse.x - root.positionFor(root.lowerValue))
                    const upperDistance = Math.abs(mouse.x - root.positionFor(root.upperValue))
                    root.activeHandle = lowerDistance <= upperDistance ? 0 : 1
                    root.gestureActive = true
                    root.gestureStarted()
                    root.updateHandle(mouse.x)
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        root.updateHandle(mouse.x)
                }
                onReleased: {
                    if (!root.gestureActive)
                        return
                    root.gestureActive = false
                    root.activeHandle = -1
                    root.gestureFinished()
                }
                onCanceled: {
                    if (!root.gestureActive)
                        return
                    root.gestureActive = false
                    root.activeHandle = -1
                    root.gestureFinished()
                }
            }
        }
    }
}
