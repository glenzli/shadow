pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string title
    property string summary
    property string toolTipText: ""
    property bool expanded: true
    property bool sectionEnabled: true
    default property alias contentData: body.data

    // The inspector is a ColumnLayout of sections.  Give it a complete,
    // explicit height contract so the next header never paints over a trailing
    // label or slider from the preceding group.
    readonly property int expandedBodyHeight: expanded ? Math.ceil(body.implicitHeight) : 0
    implicitWidth: 280
    implicitHeight: header.height
        + (expanded ? expandedBodyHeight + 8 : 0) + divider.height

    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 32
        color: headerMouse.containsMouse ? Theme.surfaceSubtle : Theme.transparent

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 12
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: root.title
                color: root.sectionEnabled ? Theme.textPrimary : Theme.textDisabled
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 0.45
            }

            Label {
                visible: root.summary.length > 0 && !root.expanded
                text: root.summary
                color: Theme.textMuted
                font.pixelSize: 9
                elide: Text.ElideRight
            }

            ShadowIcon {
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                source: "qrc:/icons/chevron-down.svg"
                color: root.sectionEnabled ? Theme.textSecondary : Theme.textDisabled
                rotation: root.expanded ? 180 : 0
            }
        }

        MouseArea {
            id: headerMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: root.sectionEnabled
                ? Qt.PointingHandCursor : Qt.ArrowCursor
            enabled: root.sectionEnabled
            Accessible.role: Accessible.Button
            Accessible.name: root.title
            Accessible.checked: root.expanded
            onClicked: root.expanded = !root.expanded
        }

        ToolTip {
            id: toolTip

            parent: header
            visible: headerMouse.containsMouse && root.toolTipText.length > 0
            delay: 500
            timeout: 5000
            text: root.toolTipText
            x: Math.max(8, Math.round((header.width - width) / 2))
            y: header.height + 6

            contentItem: Label {
                text: toolTip.text
                color: Theme.textPrimary
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            background: Rectangle {
                radius: Theme.compactControlRadius
                color: Theme.panelRaised
                border.width: 1
                border.color: Theme.borderStrong
            }
        }
    }

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.topMargin: root.expanded ? 6 : 0
        visible: root.expanded
        spacing: 0
    }

    Rectangle {
        id: divider
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }
}
