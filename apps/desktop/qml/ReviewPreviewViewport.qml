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

    function resetView() { view.reset() }
    function zoomAt(x, y, value) {
        if (!imageReady)
            return
        view.zoomAt(x, y, value)
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

        ShadowButton {
            objectName: "reviewPreviewZoomOut"
            text: "−"
            compact: true
            minimumButtonWidth: 30
            accessibleName: qsTr("Zoom out preview")
            toolTipText: accessibleName
            enabled: viewport.imageReady && !viewport.fitView
            onClicked: viewport.zoomBy(1 / 1.5)
        }
        ShadowButton {
            objectName: "reviewPreviewZoomIn"
            text: "+"
            compact: true
            minimumButtonWidth: 30
            accessibleName: qsTr("Zoom in preview")
            toolTipText: accessibleName
            enabled: viewport.imageReady && (viewport.fitView || viewport.zoomFactor < 4)
            onClicked: viewport.zoomBy(1.5)
        }
        ShadowButton {
            objectName: "reviewPreviewFit"
            text: qsTr("Fit")
            compact: true
            toolTipText: qsTr("Fit preview to window")
            enabled: viewport.imageReady
            onClicked: viewport.resetView()
        }
    }
}
