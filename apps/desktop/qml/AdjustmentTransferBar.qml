pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Global entry and activity surface. The controller owns the clipboard and
// batch lifetime, so changing workspaces never cancels an admitted operation.
Rectangle {
    id: bar
    objectName: "adjustmentTransferBar"

    property var transferController: null
    property var editor: null
    property var targets: []
    readonly property bool available: transferController !== null
    readonly property bool admissionEnabled: available
        && transferController.admissionEnabled !== false
    readonly property bool editorActive: editor !== null && editor.active
    readonly property bool busy: available
        && (transferController.busy || transferController.copying)
    readonly property bool hasClipboard: available && transferController.hasClipboard
    readonly property bool hasResults: available && transferController.results.length > 0
    readonly property int processed: available
        ? transferController.completed + transferController.failed : 0
    readonly property bool copyScopeAvailable: !transferDialog.selectedOnly
        || transferDialog.canCopySelected
    readonly property bool canCopy: admissionEnabled && editorActive
        && !editor.stateBusy && !busy && copyScopeAvailable
    readonly property var destinationTargets: {
        const values = []
        const seen = ({})
        const sourceId = hasClipboard ? String(transferController.sourcePhotoId) : ""
        const selection = targets || []
        for (let index = 0; index < selection.length; ++index) {
            const target = selection[index]
            const photoId = String(target.photoId || "")
            if (photoId.length === 0 || photoId === sourceId || seen[photoId])
                continue
            seen[photoId] = true
            values.push({
                "photoId": photoId,
                "sourcePath": String(target.sourcePath || ""),
                "title": String(target.title || "")
            })
        }
        return values
    }

    implicitHeight: content.implicitHeight + 16
    color: Theme.panel
    visible: available && (editorActive || hasClipboard || busy || hasResults)

    function copyAdjustments() {
        if (canCopy)
            transferController.copyAdjustments(
                transferDialog.selectedOnly, transferDialog.allowSpatialMasks)
    }

    function openCopySettings() {
        if (admissionEnabled && editorActive && !editor.stateBusy && !busy)
            transferDialog.presentCopy()
    }

    function openResults() {
        transferDialog.presentResults()
    }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.border
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item {
                id: copyAction
                implicitWidth: copyAdjustmentsButton.implicitWidth
                implicitHeight: copyAdjustmentsButton.implicitHeight

                ShadowButton {
                    id: copyAdjustmentsButton
                    objectName: "copyAdjustmentsButton"
                    anchors.fill: parent
                    compact: true
                    text: qsTr("Copy adjustments")
                    enabled: bar.canCopy
                    toolTipText: !bar.copyScopeAvailable
                        ? qsTr("Select an adjustment node or choose All in Copy options") : ""
                    onClicked: bar.copyAdjustments()
                }

                HoverHandler { id: copyScopeHover }

                ToolTip {
                    objectName: "copyAdjustmentsUnavailableToolTip"
                    parent: copyAction
                    visible: copyScopeHover.hovered && !copyAdjustmentsButton.enabled
                        && copyAdjustmentsButton.toolTipText.length > 0
                    delay: 450
                    text: copyAdjustmentsButton.toolTipText
                }
            }

            ShadowIconButton {
                objectName: "copyAdjustmentOptionsButton"
                buttonSize: 30
                iconSize: 16
                source: "qrc:/icons/settings.svg"
                toolTipText: qsTr("Copy options")
                enabled: bar.admissionEnabled && bar.editorActive
                    && !bar.editor.stateBusy && !bar.busy
                onClicked: bar.openCopySettings()
            }

            ShadowButton {
                objectName: "pasteAdjustmentsButton"
                compact: true
                visible: bar.editorActive
                text: qsTr("Paste adjustments")
                enabled: bar.admissionEnabled && bar.hasClipboard && !bar.busy
                    && bar.editorActive && !bar.editor.stateBusy
                    && String(bar.editor.photoId) !== String(bar.transferController.sourcePhotoId)
                toolTipText: qsTr("Append copied adjustments and preserve this photo's existing edits")
                onClicked: bar.transferController.pasteToCurrentPhoto()
            }

            ShadowButton {
                objectName: "applyAdjustmentsToSelectionButton"
                compact: true
                visible: bar.hasClipboard && bar.destinationTargets.length > 0
                text: qsTr("Apply to %1 selected").arg(bar.destinationTargets.length)
                variant: ShadowButton.Tinted
                enabled: bar.admissionEnabled && !bar.busy
                toolTipText: qsTr("Append copied adjustments to the selected photos; the source photo is excluded")
                onClicked: bar.transferController.applyToPhotos(bar.destinationTargets)
            }

            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 60
                text: bar.available && bar.transferController.statusText.length > 0
                    ? bar.transferController.statusText : bar.hasClipboard
                        ? qsTr("%1 · %2 nodes").arg(bar.transferController.sourceName)
                            .arg(bar.transferController.nodeCount)
                        : qsTr("Copy adjustments from an open photo")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideRight
                ToolTip.visible: sourceHover.hovered && text.length > 0
                ToolTip.text: bar.hasClipboard
                    ? qsTr("%1 · %2 nodes").arg(bar.transferController.sourceName)
                        .arg(bar.transferController.nodeCount) : text
                HoverHandler { id: sourceHover }
            }

            BusyIndicator {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                visible: bar.busy
                running: visible
            }

            ShadowButton {
                objectName: "cancelAdjustmentTransferButton"
                compact: true
                visible: bar.available && bar.transferController.busy
                    && !bar.transferController.copying
                text: qsTr("Cancel")
                onClicked: bar.transferController.cancel()
            }

            ShadowButton {
                objectName: "adjustmentTransferResultsButton"
                compact: true
                visible: bar.hasResults
                text: bar.available && bar.transferController.failed > 0
                    ? qsTr("Results · %1 need attention").arg(bar.transferController.failed)
                    : qsTr("Results")
                onClicked: bar.openResults()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: bar.available && bar.transferController.busy
                && !bar.transferController.copying && bar.transferController.total > 0
            spacing: 10

            ProgressBar {
                objectName: "adjustmentTransferProgress"
                Layout.preferredWidth: 160
                palette.highlight: Theme.accent
                from: 0
                to: bar.available ? Math.max(1, bar.transferController.total) : 1
                value: bar.processed
                Accessible.name: qsTr("Adjustment transfer progress")
            }

            Label {
                objectName: "adjustmentTransferProgressLabel"
                text: bar.available ? qsTr("%1 / %2")
                    .arg(bar.processed).arg(bar.transferController.total) : ""
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
            }

            Label {
                Layout.fillWidth: true
                text: bar.available ? bar.transferController.currentPhotoName : ""
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideMiddle
            }
        }
    }

    AdjustmentTransferDialog {
        id: transferDialog
        transferController: bar.transferController
        editor: bar.editor
    }
}
