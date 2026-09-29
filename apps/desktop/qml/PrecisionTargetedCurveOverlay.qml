pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var editor
    property bool interactionEnabled: true
    readonly property var tool: editor && editor.targetedCurve !== undefined ? editor.targetedCurve : null
    visible: Boolean(tool && tool.active) && interactionEnabled
    onVisibleChanged: {
        if (!visible && tool && tool.dragging)
            Qt.callLater(() => { if (!root.visible && root.tool && root.tool.dragging) root.tool.finish(true) })
    }
    Keys.onEscapePressed: event => {
        if (tool.dragging) tool.finish(true)
        else tool.active = false
        event.accepted = true
    }
    MouseArea {
        objectName: "targetedCurveInput"
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        enabled: root.visible && Boolean(root.tool && (root.tool.ready || root.tool.dragging))
        cursorShape: Qt.SizeVerCursor
        property real startY: 0
        onPressed: mouse => {
            mouse.accepted = root.tool.begin(mouse.x / width, mouse.y / height)
            if (mouse.accepted) { startY = mouse.y; root.forceActiveFocus() }
        }
        onPositionChanged: mouse => {
            if (pressed && root.tool.dragging) root.tool.move((mouse.y - startY) / height)
            else root.tool.hover(mouse.x / width, mouse.y / height)
        }
        onReleased: root.tool.finish(false)
        onCanceled: root.tool.finish(true)
        onExited: { if (!root.tool.dragging) root.tool.hover(-1, -1) }
    }
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 12
        width: Math.min(parent.width - 24, statusRow.implicitWidth + 16)
        height: 36
        radius: 6
        color: Theme.panelRaised
        border.color: Theme.border
        RowLayout {
            id: statusRow
            anchors.fill: parent
            anchors.leftMargin: 8; anchors.rightMargin: 8
            spacing: 8
            BusyIndicator {
                Layout.preferredWidth: 18; Layout.preferredHeight: 18
                visible: Boolean(root.tool && root.tool.busy)
                running: visible
            }
            Label {
                Layout.fillWidth: true
                text: root.tool ? root.tool.status : ""
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideRight
            }
            ShadowIconButton {
                visible: Boolean(root.tool && !root.tool.ready && !root.tool.busy)
                buttonSize: 26; iconSize: 16
                source: "qrc:/icons/redo.svg"
                toolTipText: qsTr("Retry sampling")
                onClicked: root.tool.refresh()
            }
            ShadowIconButton {
                buttonSize: 26; iconSize: 16
                source: "qrc:/icons/close.svg"
                toolTipText: qsTr("Close targeted adjustment")
                onClicked: root.tool.active = false
            }
        }
    }
}
