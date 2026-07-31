pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Application-shell index for workspace status and the action surfaces that
// belong beside it. Each interactive transaction lives in a child component.
Rectangle {
    id: statusBar

    required property int workspaceIndex
    required property var controller
    required property var editor
    required property var reviewWorkspace
    required property var precisionWorkspace

    LibraryAdvancedFilterPopup {
        id: advancedFilterPopup
        controller: statusBar.controller
    }

    function fullResolutionPreparationText() {
        const path = String(editor.sourcePath).toLowerCase()
        return /\.(jpe?g|heic|heif)$/.test(path)
            ? qsTranslate("Main", "Loading full-resolution image…")
            : qsTranslate("Main", "Parsing full-resolution RAW…")
    }

    height: statusBar.workspaceIndex === 0 ? 40 : 30
    color: Theme.chrome
    border.color: Theme.border

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 7

        BusyIndicator {
            Layout.preferredWidth: 15
            Layout.preferredHeight: 15
            visible: statusBar.workspaceIndex !== 1
                ? statusBar.controller.scanning
                    || statusBar.controller.refreshing
                    || statusBar.controller.busy
                    || statusBar.controller.loadingMore
                    || statusBar.controller.comparisonBusy
                    || statusBar.controller.decisionBusy
                : statusBar.editor.busy
                    || statusBar.editor.fullResolutionPreparing
            running: visible
        }

        MainLibraryFilterBar {
            visible: statusBar.workspaceIndex === 0
            Layout.alignment: Qt.AlignVCenter
            controller: statusBar.controller
            onAdvancedFilterRequested: advancedFilterPopup.open()
        }

        Label {
            Layout.fillWidth: true
            text: statusBar.workspaceIndex === 0
                ? qsTranslate("Main", "%L1 / %L2 photos").arg(
                    statusBar.controller.filteredItemCount
                ).arg(statusBar.controller.itemCount)
                : statusBar.workspaceIndex !== 1
                    ? (statusBar.controller.decisionBusy
                        ? statusBar.controller.decisionStatusText
                        : statusBar.controller.comparisonBusy
                            ? statusBar.controller.comparisonStatusText
                            : statusBar.controller.statusText)
                    : statusBar.editor.fullResolutionPreparing
                        ? statusBar.fullResolutionPreparationText()
                        : statusBar.editor.statusText
            color: Theme.textMuted
            font.pixelSize: 10
            elide: Text.ElideRight
        }

        Rectangle {
            visible: statusBar.workspaceIndex === 1
                && statusBar.editor.active
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 1
            Layout.preferredHeight: 16
            color: Theme.border
        }

        MainPrecisionProxyStatus {
            visible: statusBar.workspaceIndex === 1
                && statusBar.editor.active
            Layout.alignment: Qt.AlignVCenter
            active: statusBar.precisionWorkspace.proxyActive
        }

        Label {
            visible: statusBar.workspaceIndex !== 0
            text: statusBar.workspaceIndex === 1
                ? qsTranslate("Main", "PRECISION")
                : qsTranslate("Main", "LIBRARY")
            color: Theme.textFaint
            font.pixelSize: 9
            font.letterSpacing: 0.8
        }
    }
}
