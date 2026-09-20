pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Display-only inspection of the already selected review preview. Zoom never
// changes its request size, source identity, Recipe, or full-detail policy.
Item {
    id: viewport
    objectName: "reviewPreviewViewport"

    required property url source
    required property bool autoTransform
    required property string selectionKey
    readonly property bool fitView: view.fitView
    readonly property real zoomFactor: view.zoomFactor
    readonly property bool imageReady: preview.status === Image.Ready
    readonly property bool imageFailed: preview.status === Image.Error
    readonly property real centerX: view.centerX
    readonly property real centerY: view.centerY
    property bool applyingLinkedView: false
    property var pendingLinkedView: null

    signal viewChanged(var transform)

    function currentView() {
        return { fitView: view.fitView, zoomFactor: view.zoomFactor,
            centerX: view.normalizedX(width / 2), centerY: view.normalizedY(height / 2) }
    }
    function publishView() {
        if (!applyingLinkedView && !view.applyingTransform)
            viewChanged(currentView())
    }
    function applyView(transform) {
        if (!transform)
            return
        // Keep only the latest transform if this pane's preview is still loading.
        pendingLinkedView = imageReady ? null : transform
        if (!imageReady)
            return
        applyingLinkedView = true
        pan.cancelFlick()
        if (transform.fitView) {
            view.reset()
        } else {
            view.zoomAt(width / 2, height / 2, transform.zoomFactor)
            view.place(transform.centerX, transform.centerY, width / 2, height / 2)
        }
        applyingLinkedView = false
    }

    function resetView() {
        pendingLinkedView = null
        view.reset()
        publishView()
    }
    function zoomAt(x, y, value) {
        if (!imageReady)
            return
        view.zoomAt(x, y, value)
        publishView()
    }
    function zoomBy(factor) {
        const value = (view.fitView ? view.fitScale : view.zoomFactor) * factor
        if (factor < 1 && value <= view.fitScale)
            resetView()
        else
            zoomAt(width / 2, height / 2, value)
    }
    function toggleZoom(x, y) {
        if (view.fitView)
            zoomAt(x, y, view.fitScale * 2)
        else
            resetView()
    }

    onSelectionKeyChanged: resetView()
    onImageReadyChanged: {
        if (imageReady && pendingLinkedView)
            applyView(pendingLinkedView)
    }
    onVisibleChanged: if (!visible) resetView()
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
            zoomBy(1.5)
            event.accepted = true
        } else if (event.key === Qt.Key_Minus) {
            zoomBy(1 / 1.5)
            event.accepted = true
        } else if (event.key === Qt.Key_Escape && !fitView) {
            resetView()
            event.accepted = true
        }
    }

    PrecisionViewportState {
        id: view
        flickable: pan
        imagePixelWidth: Math.max(1, preview.implicitWidth)
        imagePixelHeight: Math.max(1, preview.implicitHeight)
    }

    Flickable {
        id: pan
        objectName: "reviewPreviewPan"
        anchors.fill: parent
        clip: true
        contentWidth: view.contentWidth
        contentHeight: view.contentHeight
        boundsBehavior: Flickable.StopAtBounds
        interactive: viewport.imageReady && !view.fitView && !view.continuousZoomActive
        // Only a user's drag/flick publishes position. Layout and linked writes
        // may update content offsets after bindings settle; never echo those.
        onContentXChanged: if (moving) viewport.publishView()
        onContentYChanged: if (moving) viewport.publishView()
        onMovementEnded: viewport.publishView()

        Image {
            id: preview
            objectName: "reviewPreviewImage"
            x: view.imageX
            y: view.imageY
            width: view.imageWidth
            height: view.imageHeight
            source: viewport.source
            autoTransform: viewport.autoTransform
            fillMode: Image.Stretch
            asynchronous: true
            cache: true
            smooth: true
            mipmap: true
            sourceSize: Qt.size(2048, 2048)
        }
    }

    MouseArea {
        enabled: viewport.imageReady
        acceptedButtons: Qt.LeftButton
        // Keep this inside the Flickable's input filtering so a drag can take
        // over the press while a double click remains a local zoom action.
        parent: pan.contentItem
        x: pan.contentX
        y: pan.contentY
        width: pan.width
        height: pan.height
        onPressed: viewport.forceActiveFocus()
        onDoubleClicked: mouse => viewport.toggleZoom(mouse.x, mouse.y)
    }

    PrecisionCanvasZoomInput {
        anchors.fill: parent
        interactionEnabled: viewport.imageReady && viewport.visible
        toolActive: false
        fitView: view.fitView
        zoomFactor: view.zoomFactor
        fitZoomFactor: view.fitScale
        onContinuousZoomStarted: view.beginContinuousZoom()
        onContinuousZoomRequested: (x, y, value) => viewport.zoomAt(x, y, value)
        onContinuousZoomFinished: view.finishContinuousZoom()
    }

    Row {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        spacing: 4
        visible: viewport.selectionKey.length > 0

        ShadowIconButton {
            objectName: "reviewPreviewZoomOut"
            source: "qrc:/icons/zoom-out.svg"
            variant: ShadowIconButton.Secondary
            accessibleName: qsTr("Zoom out preview")
            toolTipText: accessibleName
            enabled: viewport.imageReady && !viewport.fitView
            onClicked: viewport.zoomBy(1 / 1.5)
        }
        ShadowIconButton {
            objectName: "reviewPreviewZoomIn"
            source: "qrc:/icons/zoom-in.svg"
            variant: ShadowIconButton.Secondary
            accessibleName: qsTr("Zoom in preview")
            toolTipText: accessibleName
            enabled: viewport.imageReady && (viewport.fitView || viewport.zoomFactor < 4)
            onClicked: viewport.zoomBy(1.5)
        }
        ShadowButton {
            objectName: "reviewPreviewActualSize"
            text: "100%"
            accessibleName: qsTr("Preview 100%")
            compact: true
            minimumButtonWidth: 64
            selected: !viewport.fitView && Math.abs(viewport.zoomFactor - 1.0) < 0.001
            toolTipText: qsTr("Show preview pixels at 100%; use Precision for original detail")
            enabled: viewport.imageReady
            onClicked: viewport.zoomAt(viewport.width / 2, viewport.height / 2, 1)
        }
        ShadowIconButton {
            objectName: "reviewPreviewFit"
            source: "qrc:/icons/fit-view.svg"
            variant: ShadowIconButton.Secondary
            accessibleName: qsTr("Fit")
            selected: viewport.fitView
            toolTipText: qsTr("Fit preview to window")
            enabled: viewport.imageReady
            onClicked: viewport.resetView()
        }
    }
}
