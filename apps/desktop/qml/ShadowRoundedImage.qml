pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

// Reusable image presentation with a real alpha-mask corner clip. QML's
// ordinary `clip` property clips only to rectangular bounds, even when its
// Rectangle owner has a radius.
Item {
    id: roundedImage

    property url source
    property int fillMode: Image.PreserveAspectFit
    property bool asynchronous: true
    property bool cache: true
    property bool mipmap: true
    property bool autoTransform: false
    property real radius: 0
    property size requestedSourceSize: Qt.size(0, 0)
    readonly property int status: sourceImage.status
    readonly property real paintedWidth: sourceImage.paintedWidth
    readonly property real paintedHeight: sourceImage.paintedHeight

    Image {
        id: sourceImage

        anchors.fill: parent
        source: roundedImage.source
        fillMode: roundedImage.fillMode
        asynchronous: roundedImage.asynchronous
        cache: roundedImage.cache
        smooth: roundedImage.smooth
        mipmap: roundedImage.mipmap
        // Remote previews explicitly declare whether their encoded pixels
        // still carry an image-format orientation transform. Do not interpret
        // the Catalog provider orientation as EXIF here: RAW providers use a
        // different code space and generated proxies are already normalized.
        autoTransform: roundedImage.autoTransform
        sourceSize: roundedImage.requestedSourceSize
        visible: roundedImage.radius <= 0
    }

    Rectangle {
        id: roundedMask

        anchors.fill: parent
        radius: roundedImage.radius
        color: "white"
        visible: false
        layer.enabled: true
        antialiasing: true
    }

    MultiEffect {
        anchors.fill: parent
        source: sourceImage
        visible: roundedImage.radius > 0
        maskEnabled: true
        maskSource: roundedMask
    }
}
