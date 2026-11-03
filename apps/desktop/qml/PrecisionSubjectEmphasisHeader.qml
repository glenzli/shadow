pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: header
    required property var controller
    implicitHeight: 44
    color: Theme.accentSurfaceQuiet
    visible: controller.active || controller.applied

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: qsTr("Subject emphasis")
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
            color: Theme.textPrimary
        }
        BusyIndicator {
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            visible: header.controller.busy
            running: visible
        }
        ShadowButton {
            objectName: "exitSubjectEmphasis"
            compact: true
            variant: ShadowButton.Ghost
            text: header.controller.active ? qsTr("Cancel") : qsTr("Done")
            toolTipText: header.controller.active
                ? qsTr("Exit subject emphasis without applying · Esc")
                : qsTr("Close the assistant and keep the applied node · Esc")
            enabled: !header.controller.applying
            onClicked: header.controller.dismiss()
        }
    }
}
