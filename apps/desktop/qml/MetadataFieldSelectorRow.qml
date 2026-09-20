pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns one metadata field's presentation and sidebar-selection interaction.
Rectangle {
    id: root

    required property string fieldLabel
    required property string fieldValue
    required property bool checked
    signal toggleRequested(bool checked)

    implicitHeight: 46
    radius: 8
    color: fieldMouse.containsMouse || sidebarToggle.hovered
        ? Theme.buttonGhostHover : Theme.panelRaised
    border.width: 1
    border.color: Theme.border

    MouseArea {
        id: fieldMouse
        anchors.fill: parent
        anchors.rightMargin: 44
        z: 2
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggleRequested(!root.checked)
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 10
        spacing: 12

        Label {
            Layout.preferredWidth: 150
            text: root.fieldLabel
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
        }

        Label {
            Layout.fillWidth: true
            text: root.fieldValue
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSection
            elide: Text.ElideMiddle
        }

        ShadowCheckBox {
            id: sidebarToggle
            objectName: "metadataFieldSidebarCheckBox"
            Layout.alignment: Qt.AlignVCenter
            compact: true
            text: ""
            checked: root.checked
            accessibleName: qsTranslate(
                "MetadataWindow", "Show %1 in sidebar").arg(root.fieldLabel)
            onClicked: root.toggleRequested(checked)
        }
    }
}
