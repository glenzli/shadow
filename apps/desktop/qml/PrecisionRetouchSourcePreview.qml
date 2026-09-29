pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects

// The subtree exists only while hovering with a sampled source. It reuses the
// displayed texture; pointer motion never reads pixels back to the host.
Item {
    id: preview
    required property Item sampleItem
    required property point sourcePoint
    property real amount: 0.5
    width: 1
    height: width
    readonly property size boundedTextureSize: Qt.size(
        Math.min(2048, Math.max(1, Math.ceil(width * Screen.devicePixelRatio))),
        Math.min(2048, Math.max(1, Math.ceil(height * Screen.devicePixelRatio))))
    Loader {
        anchors.fill: parent
        active: preview.visible && preview.sampleItem !== null
            && preview.sampleItem.width > 0 && preview.sampleItem.height > 0
        sourceComponent: Item {
            Rectangle {
                id: circularMask
                anchors.fill: parent
                radius: width / 2
                color: "white"
                layer.enabled: true
                layer.textureSize: preview.boundedTextureSize
                visible: false
            }
            ShaderEffectSource {
                id: donor
                anchors.fill: parent
                sourceItem: preview.sampleItem
                sourceRect: Qt.rect(preview.sourcePoint.x * preview.sampleItem.width - width / 2,
                                    preview.sourcePoint.y * preview.sampleItem.height - height / 2,
                                    width, height)
                textureSize: preview.boundedTextureSize
                live: true
                visible: false
            }
            MultiEffect {
                anchors.fill: parent
                source: donor
                maskEnabled: true
                maskSource: circularMask
                opacity: preview.amount
            }
        }
    }
}
