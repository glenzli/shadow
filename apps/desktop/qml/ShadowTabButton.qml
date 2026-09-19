pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

TabButton {
    id: control

    property bool active: checked
    property url iconSource
    property int iconSize: 17
    property bool showLabelWithIcon: false
    property string toolTipText: ""
    property string accessibleName: text.length > 0 ? text : toolTipText
    property int minimumTabWidth: 54
    property int underlineInset: 14
    property int underlineMaximumWidth: 0
    property int underlineBottomMargin: 0
    property bool compact: false
    property color activeColor: Theme.accent

    implicitWidth: Math.max(
        minimumTabWidth,
        contentItem.implicitWidth + 14
    )
    implicitHeight: compact ? Theme.compactControlHeight : 36
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: enabled
    focusPolicy: enabled ? Qt.StrongFocus : Qt.NoFocus
    Accessible.name: accessibleName
    Accessible.role: Accessible.PageTab
    Accessible.checked: active
    Accessible.onPressAction: control.clicked()

    readonly property color resolvedContentColor: !control.enabled
            ? Theme.navigationTextDisabled
            : control.active
                ? control.activeColor
                : control.hovered ? Theme.textPrimary : Theme.textMuted

    contentItem: Item {
        implicitWidth: (tabIcon.visible ? control.iconSize : 0)
            + (tabLabel.visible ? tabLabel.implicitWidth : 0)
            + (tabIcon.visible && tabLabel.visible ? 7 : 0)
        implicitHeight: Math.max(control.iconSize, tabLabel.implicitHeight)

        ShadowIcon {
            id: tabIcon
            anchors.verticalCenter: parent.verticalCenter
            x: tabLabel.visible ? 0 : (parent.width - width) / 2
            visible: control.iconSource.toString().length > 0
            source: control.iconSource
            color: control.resolvedContentColor
            size: control.iconSize
        }

        Label {
            id: tabLabel
            anchors.left: tabIcon.visible ? tabIcon.right : parent.left
            anchors.leftMargin: tabIcon.visible ? 7 : 0
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            visible: !tabIcon.visible || control.showLabelWithIcon
            text: control.text
            color: control.resolvedContentColor
            font.pixelSize: Theme.fontSection
            font.weight: control.active ? Font.DemiBold : Font.Medium
            font.letterSpacing: 0.2
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight

            Behavior on color {
                ColorAnimation { duration: 90 }
            }
        }
    }

    background: Item {
        implicitWidth: control.minimumTabWidth
        implicitHeight: control.compact ? Theme.compactControlHeight : 36

        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            visible: !control.enabled
            radius: Theme.compactControlRadius
            color: Theme.buttonDisabledGhostSurface
            border.width: 0
        }

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: control.underlineBottomMargin
            width: {
                const naturalWidth = Math.max(
                    24, parent.width - control.underlineInset)
                return control.underlineMaximumWidth > 0
                    ? Math.min(naturalWidth, control.underlineMaximumWidth)
                    : naturalWidth
            }
            height: 2
            radius: 1
            color: control.activeColor
            opacity: control.enabled && (control.active || control.visualFocus)
                ? 1 : 0

            Behavior on opacity {
                NumberAnimation { duration: 90 }
            }
        }
    }

    ToolTip {
        id: toolTip

        parent: control
        visible: control.enabled && control.hovered
            && (control.toolTipText.length > 0 || tabLabel.truncated)
        delay: 450
        timeout: 4000
        text: control.toolTipText.length > 0
            ? control.toolTipText : control.text
        x: Math.round((control.width - width) / 2)
        y: control.height + 6

        contentItem: Label {
            text: toolTip.text
            color: Theme.textPrimary
            font.pixelSize: Theme.fontMeta
            font.weight: Font.Medium
        }

        background: Rectangle {
            radius: Theme.compactControlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
    }
}
