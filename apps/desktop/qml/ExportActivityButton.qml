pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// The controller keeps job state across workspaces and while its dialog is hidden.
ShadowButton {
    id: activity

    required property var exportController

    objectName: "exportActivityButton"
    visible: exportController.busy || exportController.totalCount > 0
    compact: true
    implicitWidth: Math.min(180, Math.max(68, contentItem.implicitWidth + 24))
    variant: ShadowButton.Secondary
    text: exportController.busy
        ? qsTr("Exporting %1/%2").arg(exportController.currentCount)
            .arg(exportController.totalCount)
        : exportController.statusText
    toolTipText: exportController.statusText
    accessibleName: text

    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        height: 2
        radius: 1
        visible: activity.exportController.busy
        width: parent.width * Math.max(0, Math.min(1,
            activity.exportController.currentCount
                / Math.max(1, activity.exportController.totalCount)))
        color: Theme.accent
    }
}
