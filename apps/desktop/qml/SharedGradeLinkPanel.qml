pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    objectName: "sharedGradeLinkPanel"
    property var linkController: null
    property var editor: null
    property var targets: []
    readonly property bool available: linkController !== null
    readonly property bool hasActivity: available
        && (linkController.busy || linkController.results.length > 0 || linkController.statusText.length > 0)

    visible: hasActivity
    implicitHeight: visible ? content.implicitHeight + 16 : 0
    color: Theme.panel

    function openResults() { resultDialog.presentResults() }

    Rectangle { width: parent.width; height: 1; color: Theme.border }
    RowLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 8
        BusyIndicator {
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            visible: panel.available && panel.linkController.busy
            running: visible
        }
        Label {
            objectName: "sharedGradeLinkActivityLabel"
            Layout.fillWidth: true
            Layout.minimumWidth: 40
            text: panel.available ? panel.linkController.statusText : ""
            color: Theme.textSecondary
            font.pixelSize: Theme.fontMeta
            elide: Text.ElideRight
            ToolTip.visible: statusHover.hovered && text.length > 0
            ToolTip.text: text
            HoverHandler { id: statusHover }
        }
        ShadowButton {
            objectName: "stopSharedGradeLinkActivityButton"
            compact: true
            visible: panel.available && panel.linkController.busy
            text: qsTr("Stop")
            onClicked: panel.linkController.cancel()
        }
        ShadowButton {
            objectName: "sharedGradeLinkResultsButton"
            compact: true
            text: qsTr("Shared node results")
            onClicked: panel.openResults()
        }
    }
    SharedGradeLinkDialog {
        id: resultDialog
        linkController: panel.linkController
        editor: panel.editor
    }
    Connections {
        target: panel.linkController
        ignoreUnknownSignals: true
        function onPresentationRequested(mode) { resultDialog.presentInput(mode, panel.targets) }
    }
}
