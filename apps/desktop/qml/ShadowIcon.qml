pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

Item {
    id: root

    property url source
    property color color: Theme.textSecondary
    property int size: 18
    property bool mirrored: false

    readonly property alias status: image.status

    implicitWidth: size
    implicitHeight: size
    Accessible.ignored: true

    Image {
        id: image

        anchors.centerIn: parent
        width: Math.min(root.width, root.size)
        height: Math.min(root.height, root.size)
        source: root.source
        sourceSize: Qt.size(
            Math.ceil(root.size * Screen.devicePixelRatio),
            Math.ceil(root.size * Screen.devicePixelRatio)
        )
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        mirror: root.mirrored
        visible: root.source.toString().length > 0

        layer.enabled: visible
        layer.smooth: true
        layer.effect: MultiEffect {
            colorization: 1.0
            colorizationColor: root.color
        }
    }

    Behavior on color {
        ColorAnimation { duration: 80 }
    }
}
