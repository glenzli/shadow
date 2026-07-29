pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

// Display-only projection of the renderer's exact R8 local-mask coverage.
// Geometry and condition semantics remain native-owned; this component only
// verifies the paired preview identity and applies the theme tint.
Item {
    id: overlay

    required property var editor
    required property bool interactionEnabled
    required property bool coverageVisible
    required property string readyPreviewGeneration

    readonly property var mask: editor.selectedLocalMask
    readonly property int kind: Number(mask.kind || 0)
    readonly property string coverageSource: String(editor.maskCoverageSource || "")
    readonly property string pairedPreviewGeneration: queryValue(coverageSource, "preview")
    readonly property bool generationMatches: readyPreviewGeneration.length > 0
                                              && pairedPreviewGeneration === readyPreviewGeneration
    readonly property bool coverageAvailable: interactionEnabled
        && editor.active && editor.hasSelectedGradeNode && kind >= 1
        && kind <= 5 && coverageSource.length > 0 && generationMatches
    readonly property bool coverageReady: coverageAvailable
        && coverageImage.status === Image.Ready

    visible: coverageAvailable && coverageVisible

    function queryValue(source, key) {
        const expression = new RegExp("[?&]" + key + "=([^&#]+)")
        const match = String(source).match(expression)
        return match && match.length > 1 ? decodeURIComponent(match[1]) : ""
    }

    Image {
        id: coverageImage

        anchors.fill: parent
        source: overlay.coverageAvailable ? overlay.coverageSource : ""
        fillMode: Image.Stretch
        asynchronous: true
        cache: false
        retainWhileLoading: false
        smooth: true
        mipmap: false
        visible: overlay.coverageReady && overlay.coverageVisible
        opacity: Theme.maskCoverageTint.a

        layer.enabled: visible
        layer.smooth: true
        layer.effect: MultiEffect {
            colorization: 1.0
            colorizationColor: Qt.rgba(Theme.maskCoverageTint.r, Theme.maskCoverageTint.g,
                                       Theme.maskCoverageTint.b, 1.0)
        }
    }
}
