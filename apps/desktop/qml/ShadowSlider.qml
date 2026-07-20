pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: field

    required property string label
    property alias value: slider.value
    property alias from: slider.from
    property alias to: slider.to
    property alias stepSize: slider.stepSize
    property int decimals: 2
    property string suffix: ""
    property color accent: "#d8b36a"
    property color textPrimary: "#edf0f2"
    property color textMuted: "#8b949e"
    signal edited(real value)
    signal gestureStarted()
    signal gestureFinished()

    implicitHeight: 58

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: field.label
                color: field.textPrimary
                font.pixelSize: 11
            }
            Label {
                text: Number(slider.value).toFixed(field.decimals) + field.suffix
                color: field.textMuted
                font.family: "Menlo"
                font.pixelSize: 10
            }
        }

        Slider {
            id: slider
            Layout.fillWidth: true
            snapMode: Slider.SnapAlways
            onMoved: field.edited(value)
            onPressedChanged: {
                if (pressed)
                    field.gestureStarted()
                else
                    field.gestureFinished()
            }

            background: Rectangle {
                x: slider.leftPadding
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                width: slider.availableWidth
                height: 3
                radius: 1.5
                color: "#30363d"

                Rectangle {
                    width: slider.visualPosition * parent.width
                    height: parent.height
                    radius: parent.radius
                    color: field.accent
                }
            }

            handle: Rectangle {
                x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                implicitWidth: 13
                implicitHeight: 13
                radius: 6.5
                color: slider.pressed ? "#f0ce89" : field.accent
                border.color: "#17130d"
            }
        }
    }
}
