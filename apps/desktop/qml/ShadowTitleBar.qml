pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window

ToolBar {
    id: titleBar
    required property var hostWindow
    default property alias titleContent: content.data

    objectName: "titleToolBar"
    implicitHeight: 44
    topPadding: 0
    bottomPadding: 0
    leftPadding: Math.max(SafeArea.margins.left,
        Qt.platform.os === "osx" && hostWindow.visibility !== Window.FullScreen ? 96 : 16)
    rightPadding: Math.max(SafeArea.margins.right,
        Qt.platform.os === "windows" ? 152 : 16)

    background: Rectangle {
        color: Theme.chrome
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.border
        }
    }
    contentItem: Item {
        id: content
        Item {
            anchors.fill: parent
            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                onActiveChanged: {
                    if (active)
                        titleBar.hostWindow.startSystemMove()
                }
            }
        }
    }
}
