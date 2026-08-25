pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Application-shell index for workspace status and the action surfaces that
// belong beside it. Each interactive transaction lives in a child component.
Rectangle {
    id: statusBar

    required property int workspaceIndex
    required property int reviewWorkspaceIndex
    required property int precisionWorkspaceIndex
    required property int mapWorkspaceIndex
    required property int peopleWorkspaceIndex
    required property var controller
    required property var editor
    required property var peopleAnalysisController
    required property var semanticSearchController
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

    height: statusBar.workspaceIndex === statusBar.reviewWorkspaceIndex ? 40 : 30
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
            visible: statusBar.workspaceIndex === statusBar.reviewWorkspaceIndex
                ? statusBar.semanticSearchController.busy
                    || statusBar.controller.scanning
                    || statusBar.controller.refreshing
                    || statusBar.controller.busy
                    || statusBar.controller.loadingMore
                    || statusBar.controller.comparisonBusy
                    || statusBar.controller.decisionBusy
                : statusBar.workspaceIndex === statusBar.mapWorkspaceIndex
                    ? statusBar.reviewWorkspace.controller.libraryMapBusy
                        || statusBar.reviewWorkspace.libraryWebMapController.busy
                : statusBar.workspaceIndex !== statusBar.precisionWorkspaceIndex
                ? statusBar.workspaceIndex === statusBar.peopleWorkspaceIndex
                    ? statusBar.peopleAnalysisController.busy
                    : statusBar.controller.scanning
                        || statusBar.controller.refreshing
                        || statusBar.controller.busy
                        || statusBar.controller.loadingMore
                        || statusBar.controller.comparisonBusy
                        || statusBar.controller.decisionBusy
                : statusBar.editor.busy
                    || statusBar.precisionWorkspace
                        .foregroundFullResolutionPending
            running: visible
        }

        MainLibraryFilterBar {
            visible: statusBar.workspaceIndex === statusBar.reviewWorkspaceIndex
            Layout.alignment: Qt.AlignVCenter
            controller: statusBar.controller
            semanticSearchController: statusBar.semanticSearchController
            onAdvancedFilterRequested: advancedFilterPopup.open()
        }

        Label {
            Layout.fillWidth: true
            text: statusBar.workspaceIndex === statusBar.reviewWorkspaceIndex
                ? statusBar.semanticSearchController.errorText.length > 0
                    ? statusBar.semanticSearchController.errorText
                    : statusBar.semanticSearchController.busy
                        || statusBar.semanticSearchController.hasResults
                        ? statusBar.semanticSearchController.statusText
                        : qsTranslate("Main", "%L1 / %L2 photos").arg(
                            statusBar.controller.filteredItemCount
                        ).arg(statusBar.controller.itemCount)
                : statusBar.workspaceIndex === statusBar.mapWorkspaceIndex
                    ? qsTranslate("ReviewWorkspace", "%L1 photos in view").arg(
                        statusBar.reviewWorkspace.controller.libraryMapPhotoCount)
                : statusBar.workspaceIndex === statusBar.peopleWorkspaceIndex
                    ? statusBar.peopleAnalysisController.statusText
                : statusBar.workspaceIndex !== statusBar.precisionWorkspaceIndex
                    ? (statusBar.controller.decisionBusy
                        ? statusBar.controller.decisionStatusText
                        : statusBar.controller.comparisonBusy
                            ? statusBar.controller.comparisonStatusText
                            : statusBar.controller.statusText)
                    : statusBar.precisionWorkspace
                        .foregroundFullResolutionPending
                        ? statusBar.fullResolutionPreparationText()
                        : statusBar.editor.statusText
            color: statusBar.workspaceIndex === statusBar.reviewWorkspaceIndex
                && statusBar.semanticSearchController.errorText.length > 0
                ? Theme.errorText : Theme.textMuted
            font.pixelSize: 10
            elide: Text.ElideRight
        }

        Rectangle {
            visible: statusBar.workspaceIndex === statusBar.precisionWorkspaceIndex
                && statusBar.editor.active
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 1
            Layout.preferredHeight: 16
            color: Theme.border
        }

        MainPrecisionProxyStatus {
            visible: statusBar.workspaceIndex === statusBar.precisionWorkspaceIndex
                && statusBar.editor.active
            Layout.alignment: Qt.AlignVCenter
            active: statusBar.precisionWorkspace.proxyActive
        }

        Label {
            visible: statusBar.workspaceIndex !== statusBar.reviewWorkspaceIndex
            text: statusBar.workspaceIndex === statusBar.precisionWorkspaceIndex
                ? qsTranslate("Main", "PRECISION")
                : statusBar.workspaceIndex === statusBar.mapWorkspaceIndex
                    ? qsTranslate("Main", "MAP")
                : statusBar.workspaceIndex === statusBar.peopleWorkspaceIndex
                    ? qsTranslate("Main", "PEOPLE")
                    : qsTranslate("Main", "LIBRARY")
            color: Theme.textFaint
            font.pixelSize: 9
            font.letterSpacing: 0.8
        }
    }
}
