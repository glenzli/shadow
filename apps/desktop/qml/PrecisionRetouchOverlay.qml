pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick

// Composes photo-local retouch regions above the Precision viewport. Continuous brush
// coverage and legacy spot interaction remain separate owners so this entry stays an index.
Item {
    id: overlay

    required property var editor
    required property bool interactionEnabled
    required property real levelZeroWidth
    required property real levelZeroHeight

    readonly property real pixelScale: Math.max(
        0.0001,
        Math.min(
            width / Math.max(1, levelZeroWidth),
            height / Math.max(1, levelZeroHeight)
        )
    )

    visible: interactionEnabled && editor.active
    enabled: visible && !editor.stateBusy

    Repeater {
        model: overlay.editor.retouchStrokes

        delegate: PrecisionRetouchStrokeHandle {
            anchors.fill: parent
            z: 1
            editor: overlay.editor
            pixelScale: overlay.pixelScale
        }
    }

    Repeater {
        model: overlay.editor.retouchSpots

        delegate: PrecisionRetouchSpotHandle {
            anchors.fill: parent
            z: 2
            editor: overlay.editor
            pixelScale: overlay.pixelScale
        }
    }
}
