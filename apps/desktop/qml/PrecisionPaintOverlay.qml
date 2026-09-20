pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: overlay
    required property var editor
    required property bool interactionEnabled
    required property bool previewReady
    required property string previewGeneration
    required property real outputAspectRatio
    readonly property var paint: editor.paint
    readonly property real brushDiameter: Math.max(2, paint.displayRadius(outputAspectRatio) * width * 2
        + 0 * paint.radius + 0 * editor.parameterRevision)
    visible: interactionEnabled
    onInteractionEnabledChanged: {
        if (!interactionEnabled) { paint.cancelStroke(); paint.picking = false }
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_BracketLeft) { paint.radius = Math.max(0.0001, paint.radius / 1.2); event.accepted = true }
        else if (event.key === Qt.Key_BracketRight) { paint.radius = Math.min(0.1, paint.radius * 1.2); event.accepted = true }
        else if (event.key === Qt.Key_Escape && paint.strokeActive) { paint.cancelStroke(); event.accepted = true }
    }
    MouseArea {
        id: input
        objectName: "paintStrokeInput"
        anchors.fill: parent
        enabled: overlay.interactionEnabled && (overlay.paint.strokeActive || (overlay.previewReady && overlay.paint.canPaint))
        hoverEnabled: true
        preventStealing: true
        acceptedButtons: Qt.LeftButton
        cursorShape: overlay.paint.picking ? Qt.CrossCursor : Qt.BlankCursor
        onPressed: mouse => {
            overlay.forceActiveFocus()
            if (overlay.paint.picking || (mouse.modifiers & Qt.AltModifier)) {
                overlay.paint.sampleColor(mouse.x / width, mouse.y / height, overlay.previewGeneration)
                return
            }
            overlay.paint.beginStroke(mouse.x / width, mouse.y / height, overlay.outputAspectRatio)
        }
        onPositionChanged: mouse => {
            if (pressed && overlay.paint.strokeActive)
                overlay.paint.appendPoint(mouse.x / width, mouse.y / height, 1)
        }
        onReleased: mouse => {
            if (overlay.paint.strokeActive) {
                overlay.paint.appendPoint(mouse.x / width, mouse.y / height, 1)
                overlay.paint.finishStroke()
            }
        }
        onCanceled: overlay.paint.cancelStroke()
    }
    Rectangle {
        visible: input.containsMouse && !overlay.paint.picking
        x: input.mouseX - width / 2; y: input.mouseY - height / 2
        width: overlay.brushDiameter; height: width
        radius: width / 2; color: "transparent"
        border.color: "#dfffffff"; border.width: 1
        Rectangle {
            anchors.fill: parent; anchors.margins: 1
            radius: width / 2; color: "transparent"
            border.color: "#a0000000"; border.width: 1
        }
    }
}
