pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Effects

// Provider-neutral point-prompt input over the displayed photo. The controller
// owns the point list and asynchronous generation; this component only
// projects normalized coordinates and renders the accepted prompt snapshot.
Item {
    id: overlay

    required property bool interactionEnabled
    required property bool busy
    required property bool foregroundMode
    required property var promptPoints
    required property color foregroundColor
    required property color backgroundColor
    property string candidateSource: ""
    property bool candidateVisible: false

    property int maximumPoints: 16

    readonly property int pointCount: promptPoints
            && typeof promptPoints.length === "number"
        ? promptPoints.length : 0
    readonly property int renderedPointCount: promptRepeater.count
    readonly property bool canAddPoint: interactionEnabled
        && !busy
        && width > 0
        && height > 0
        && pointCount < maximumPoints

    signal pointRequested(real normalizedX, real normalizedY, bool foreground)
    signal undoRequested()
    signal clearRequested()

    visible: interactionEnabled
    enabled: visible
    focus: visible

    function clampNormalized(value) {
        return Math.max(0, Math.min(1, value))
    }

    function requestPoint(localX, localY) {
        if (!canAddPoint)
            return
        pointRequested(
            clampNormalized(localX / width),
            clampNormalized(localY / height),
            foregroundMode
        )
    }

    function requestUndo() {
        if (!busy && pointCount > 0)
            undoRequested()
    }

    function requestClear() {
        if (!busy && pointCount > 0)
            clearRequested()
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Backspace
                || event.key === Qt.Key_Delete) {
            requestUndo()
            event.accepted = true
        }
    }

    Image {
        id: candidateImage

        objectName: "aiMaskCandidateImage"
        anchors.fill: parent
        source: overlay.candidateVisible ? overlay.candidateSource : ""
        fillMode: Image.Stretch
        asynchronous: true
        cache: false
        retainWhileLoading: false
        smooth: true
        mipmap: false
        visible: overlay.candidateVisible
            && overlay.candidateSource.length > 0
            && status === Image.Ready
        opacity: 0.38

        layer.enabled: visible
        layer.smooth: true
        layer.effect: MultiEffect {
            colorization: 1.0
            colorizationColor: overlay.foregroundColor
        }
    }

    Repeater {
        id: promptRepeater

        model: overlay.promptPoints

        delegate: Item {
            id: marker

            required property int index
            required property var modelData

            readonly property bool foreground:
                Boolean(marker.modelData.foreground)
            readonly property color markerColor: foreground
                ? overlay.foregroundColor : overlay.backgroundColor
            readonly property color markerSurface: Qt.rgba(
                markerColor.r,
                markerColor.g,
                markerColor.b,
                0.24
            )

            objectName: "aiMaskPromptMarker" + marker.index
            x: overlay.clampNormalized(Number(marker.modelData.x))
                * overlay.width - width / 2
            y: overlay.clampNormalized(Number(marker.modelData.y))
                * overlay.height - height / 2
            width: 24
            height: 24
            opacity: overlay.busy ? 0.72 : 1

            Rectangle {
                anchors.centerIn: parent
                width: 20
                height: 20
                radius: 10
                color: marker.markerSurface
                border.width: 2
                border.color: marker.markerColor
            }

            Rectangle {
                anchors.centerIn: parent
                width: 10
                height: 2
                radius: 1
                color: marker.markerColor
            }

            Rectangle {
                anchors.centerIn: parent
                width: 2
                height: 10
                radius: 1
                color: marker.markerColor
                visible: marker.foreground
            }
        }
    }

    MouseArea {
        id: promptSurface

        objectName: "aiMaskPromptSurface"
        anchors.fill: parent
        enabled: overlay.canAddPoint
        acceptedButtons: Qt.LeftButton
        cursorShape: overlay.busy ? Qt.BusyCursor
            : overlay.canAddPoint ? Qt.CrossCursor : Qt.ArrowCursor

        onClicked: mouse => overlay.requestPoint(mouse.x, mouse.y)
    }
}
