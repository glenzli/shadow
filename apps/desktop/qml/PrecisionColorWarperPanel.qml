pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    objectName: "precisionColorWarperPanel"
    required property var editor
    signal closeRequested()
    signal widthRequested(real value)
    color: Theme.panel

    function finishEditing() { if (warper) warper.finishEditing() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 50
            Layout.leftMargin: Theme.panelPadding
            Layout.rightMargin: 10
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("Color map")
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }
            Label {
                text: "OKLab · 5 × 5"
                font.pixelSize: Theme.fontMeta
                color: Theme.textMuted
            }
            ShadowIconButton {
                objectName: "closeColorWarperPanel"
                source: "qrc:/icons/close.svg"
                toolTipText: qsTr("Return to adjustments")
                accessibleName: toolTipText
                onClicked: panel.closeRequested()
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
        ScrollView {
            id: editorScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            contentHeight: warper.implicitHeight + 32
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColorWarperEditor {
                id: warper
                objectName: "expandedColorWarperEditor"
                x: Theme.panelPadding
                y: Theme.panelPadding
                width: Math.max(0, editorScroll.availableWidth - 32)
                height: implicitHeight
                expanded: true
                availableMeshHeight: Math.max(120, editorScroll.availableHeight - 32 - 166)
                controller: panel.editor
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
        Label {
            Layout.fillWidth: true
            Layout.margins: Theme.panelPadding
            text: qsTr("Drag a point. Hold Shift for fine control; arrow keys nudge the selected point.")
            font.pixelSize: Theme.fontMeta
            color: Theme.textMuted
            wrapMode: Text.Wrap
        }
    }

    Rectangle {
        anchors.left: parent.left
        height: parent.height
        width: 1
        color: resizeHandle.containsMouse || resizeHandle.pressed ? Theme.accent : Theme.border
    }
    MouseArea {
        id: resizeHandle
        objectName: "colorWarperResizeHandle"
        anchors.left: parent.left
        width: 7
        height: parent.height
        hoverEnabled: true
        cursorShape: Qt.SplitHCursor
        property real startX: 0
        property real startWidth: 0
        onPressed: mouse => {
            panel.finishEditing()
            startX = mapToItem(null, mouse.x, mouse.y).x
            startWidth = panel.width
        }
        onPositionChanged: mouse => {
            if (pressed)
                panel.widthRequested(startWidth + startX - mapToItem(null, mouse.x, mouse.y).x)
        }
    }
    onVisibleChanged: { if (!visible) finishEditing() }
}
