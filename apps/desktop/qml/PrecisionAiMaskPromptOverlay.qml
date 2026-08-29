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
    required property bool faceRegionMode
    required property bool foregroundMode
    required property var promptPoints
    property int peopleCount: 0
    required property color foregroundColor
    required property color backgroundColor
    required property color candidateColor
    property string candidateSource: ""
    property bool candidateVisible: false

    property int maximumPoints: 16

    readonly property int pointCount: promptPoints
            && typeof promptPoints.length === "number"
        ? promptPoints.length : 0
    readonly property int renderedPointCount: promptRepeater.count
    readonly property bool candidateRendered: candidateImage.visible
    readonly property bool actionDockVisible: actionDock.visible
    readonly property string guidanceText: busy
        ? faceRegionMode
            ? qsTranslate("PrecisionWorkspace", "Identifying facial details…")
            : qsTranslate("PrecisionWorkspace", "Updating selection…")
        : candidateVisible
            ? faceRegionMode
                ? qsTranslate("PrecisionWorkspace", "Facial detail selected · confirm or choose another region")
                : qsTranslate("PrecisionWorkspace", "Selected area preview · add or subtract points to refine")
            : faceRegionMode
                ? peopleCount > 0
                    ? qsTranslate("PrecisionWorkspace", "Choose a person and details in the panel")
                    : qsTranslate("PrecisionWorkspace", "No person selected · retry detection")
            : pointCount > 0
                ? qsTranslate("PrecisionWorkspace", "Selection was not generated · click Retry selection")
                : foregroundMode
                        ? qsTranslate("PrecisionWorkspace", "Click the object to select it")
                        : qsTranslate("PrecisionWorkspace", "Choose Add, then click the object first")
    readonly property bool canAddPoint: interactionEnabled
        && !busy
        && width > 0
        && height > 0
        && !faceRegionMode
        && pointCount < maximumPoints

    signal pointRequested(real normalizedX, real normalizedY, bool foreground)
    signal undoRequested()
    signal clearRequested()
    signal foregroundModeRequested(bool foreground)
    signal retryRequested()
    signal applyRequested()
    signal cancelRequested()

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

    function requestForegroundMode(foreground) {
        if (interactionEnabled && !busy && !faceRegionMode)
            foregroundModeRequested(foreground)
    }

    function requestRetry() {
        if (interactionEnabled && !busy && !candidateVisible
                && (pointCount > 0 || faceRegionMode))
            retryRequested()
    }

    function requestApply() {
        if (interactionEnabled && !busy && candidateVisible)
            applyRequested()
    }

    function requestCancel() {
        if (interactionEnabled)
            cancelRequested()
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
        opacity: 0.52

        layer.enabled: visible
        layer.smooth: true
        layer.effect: MultiEffect {
            colorization: 1.0
            colorizationColor: overlay.candidateColor
        }
    }

    Rectangle {
        id: guidancePill

        objectName: "aiMaskGuidancePill"
        z: 3
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 18
        width: Math.min(parent.width - 24, guidanceRow.implicitWidth + 28)
        height: 34
        radius: 17
        color: Qt.rgba(0.05, 0.06, 0.08, 0.82)
        border.width: 1
        border.color: overlay.candidateVisible
            ? overlay.candidateColor : Qt.rgba(1, 1, 1, 0.24)

        Row {
            id: guidanceRow

            anchors.centerIn: parent
            spacing: 9

            Item {
                width: 14
                height: 14
                visible: overlay.busy

                RotationAnimator on rotation {
                    from: 0
                    to: 360
                    duration: 850
                    loops: Animation.Infinite
                    running: overlay.busy
                }

                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: Qt.rgba(1, 1, 1, 0.28)
                }

                Rectangle {
                    width: 4
                    height: 4
                    radius: 2
                    color: "white"
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                }
            }

            Rectangle {
                width: 10
                height: 10
                radius: 5
                visible: !overlay.busy
                color: overlay.candidateVisible
                    ? overlay.candidateColor
                    : overlay.foregroundMode ? overlay.foregroundColor : overlay.backgroundColor
            }

            Text {
                objectName: "aiMaskGuidanceText"
                text: overlay.guidanceText
                color: "white"
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
        }
    }

    Repeater {
        id: promptRepeater

        model: overlay.faceRegionMode ? [] : overlay.promptPoints

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
        z: 1
        anchors.fill: parent
        enabled: overlay.canAddPoint && !overlay.faceRegionMode
        acceptedButtons: Qt.LeftButton
        cursorShape: overlay.busy ? Qt.BusyCursor
            : overlay.canAddPoint ? Qt.CrossCursor : Qt.ArrowCursor

        onClicked: mouse => overlay.requestPoint(mouse.x, mouse.y)
    }

    Rectangle {
        id: actionDock

        objectName: "aiMaskActionDock"
        z: 4
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 18
        width: Math.min(parent.width - 24, actionRow.implicitWidth + 16)
        height: 40
        radius: 10
        color: Qt.rgba(0.05, 0.06, 0.08, 0.88)
        border.width: 1
        border.color: overlay.candidateVisible
            ? overlay.candidateColor : Qt.rgba(1, 1, 1, 0.24)
        visible: overlay.interactionEnabled

        Row {
            id: actionRow

            anchors.centerIn: parent
            spacing: 6

            OverlayActionButton {
                objectName: "aiMaskAddSelectionButton"
                visible: !overlay.faceRegionMode
                label: qsTranslate("PrecisionWorkspace", "Add selection (+)")
                active: overlay.foregroundMode
                accentColor: overlay.foregroundColor
                buttonEnabled: !overlay.busy
                onClicked: overlay.requestForegroundMode(true)
            }

            OverlayActionButton {
                objectName: "aiMaskSubtractSelectionButton"
                visible: !overlay.faceRegionMode
                label: qsTranslate("PrecisionWorkspace", "Subtract selection (−)")
                active: !overlay.foregroundMode
                accentColor: overlay.backgroundColor
                buttonEnabled: !overlay.busy
                onClicked: overlay.requestForegroundMode(false)
            }

            OverlayActionButton {
                objectName: "aiMaskRetrySelectionButton"
                visible: !overlay.candidateVisible
                    && (overlay.pointCount > 0
                        || (overlay.faceRegionMode && overlay.peopleCount === 0))
                label: qsTranslate("PrecisionWorkspace", "Retry selection")
                buttonEnabled: !overlay.busy
                onClicked: overlay.requestRetry()
            }

            OverlayActionButton {
                objectName: "aiMaskCancelButton"
                label: qsTranslate("PrecisionWorkspace", "Cancel")
                buttonEnabled: true
                onClicked: overlay.requestCancel()
            }

            OverlayActionButton {
                objectName: "aiMaskApplyButton"
                visible: overlay.candidateVisible
                label: qsTranslate("PrecisionWorkspace", "Apply mask")
                primary: true
                accentColor: overlay.candidateColor
                buttonEnabled: !overlay.busy
                onClicked: overlay.requestApply()
            }
        }
    }

    component OverlayActionButton: Rectangle {
        id: actionButton

        required property string label
        property bool active: false
        property bool primary: false
        property bool buttonEnabled: true
        property color accentColor: Qt.rgba(1, 1, 1, 0.14)

        signal clicked()

        implicitWidth: Math.max(58, actionLabel.implicitWidth + 20)
        implicitHeight: 28
        radius: 7
        enabled: buttonEnabled
        opacity: enabled ? 1 : 0.42
        color: primary ? accentColor
            : active ? Qt.rgba(accentColor.r, accentColor.g, accentColor.b, 0.30)
            : Qt.rgba(1, 1, 1, 0.08)
        border.width: active && !primary ? 1 : 0
        border.color: accentColor

        Text {
            id: actionLabel

            anchors.centerIn: parent
            text: actionButton.label
            color: "white"
            font.pixelSize: 11
            font.weight: actionButton.primary ? Font.DemiBold : Font.Medium
        }

        TapHandler {
            enabled: actionButton.buttonEnabled
            onTapped: actionButton.clicked()
        }

        Accessible.name: label
        Accessible.role: Accessible.Button
    }
}
