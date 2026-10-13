pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Input captures only the chosen metadata targets. Nonmodal results belong to
// the controller and survive dismissal or workspace changes.
ShadowDialog {
    id: dialog
    objectName: "sharedGradeLinkDialog"

    property var linkController: null
    property var editor: null
    property string mode: "results"
    property var frozenTargets: []
    property bool includeSelectedPhotos: true
    property bool allowSpatialMasks: false
    readonly property bool available: linkController !== null
    readonly property bool inputMode: mode !== "results"
    readonly property bool busy: available && linkController.busy
    readonly property bool canApply: available && linkController.admissionEnabled && !busy
        && (mode === "publish" ? nodeNameField.text.trim().length > 0 : frozenTargets.length > 0)

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(inputMode ? 560 : 680, Math.max(0, (parent ? parent.width : 728) - 40))
    height: Math.min(inputMode ? 520 : 580, Math.max(0, (parent ? parent.height : 628) - 40))
    modal: inputMode
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    title: mode === "publish" ? qsTr("Publish shared node")
        : mode === "link" ? qsTr("Link shared node") : qsTr("Shared node results")

    function presentInput(requestMode, targets) {
        if (!available || busy || !linkController.admissionEnabled)
            return
        mode = String(requestMode)
        const normalized = []
        const seen = ({})
        const sourceId = String(linkController.requestSourcePhotoId || "")
        const selected = targets || []
        for (let index = 0; index < selected.length; ++index) {
            const target = selected[index]
            const id = String(target.photoId || "")
            const path = String(target.sourcePath || "")
            if (id.length === 0 || id === sourceId || seen[id])
                continue
            seen[id] = true
            normalized.push({ "photoId": id, "sourcePath": path, "title": String(target.title || "") })
        }
        frozenTargets = normalized
        includeSelectedPhotos = normalized.length > 0
        nodeNameField.text = String(linkController.nodeName || "")
        open()
        if (mode === "publish") {
            nodeNameField.forceActiveFocus()
            nodeNameField.selectAll()
        }
    }
    function presentResults() {
        if (!available)
            return
        mode = "results"
        open()
    }
    function applyRequest() {
        if (!canApply)
            return
        const targets = includeSelectedPhotos ? frozenTargets : []
        if (mode === "publish")
            linkController.publish(nodeNameField.text, targets, allowSpatialMasks)
        else
            linkController.linkToPhotos(frozenTargets, allowSpatialMasks)
        mode = "results"
    }

    contentItem: ColumnLayout {
        spacing: 12

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.inputMode
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: 12
                Label {
                    objectName: "sharedGradeLinkSourceLabel"
                    Layout.fillWidth: true
                    text: dialog.available ? qsTr("From %1").arg(dialog.linkController.requestSourceName) : ""
                    color: Theme.textSecondary
                    elide: Text.ElideMiddle
                }
                ShadowTextField {
                    id: nodeNameField
                    objectName: "sharedGradeNodeNameField"
                    Layout.fillWidth: true
                    visible: dialog.mode === "publish"
                    placeholderText: qsTr("Node name")
                    Accessible.name: qsTr("Shared node name")
                    onAccepted: dialog.applyRequest()
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.mode === "publish"
                        ? qsTr("Publish the current adjustment parameters and update linked photos. Each photo keeps its own strength, visibility and masks.")
                        : qsTr("Link the published adjustment parameters to the selected photos. New masks are prepared separately for each photo.")
                    color: Theme.textPrimary
                    wrapMode: Text.WordWrap
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Local adjustment changes are kept and reported. Other variants retain their saved appearance until you update them.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }
                ShadowCheckBox {
                    objectName: "linkSelectedPhotosOption"
                    Layout.fillWidth: true
                    visible: dialog.mode === "publish"
                    text: qsTr("Link selected photos (%1)").arg(dialog.frozenTargets.length)
                    checked: dialog.includeSelectedPhotos
                    enabled: dialog.frozenTargets.length > 0
                    onToggled: dialog.includeSelectedPhotos = checked
                }
                Label {
                    Layout.fillWidth: true
                    visible: dialog.mode === "link" || dialog.frozenTargets.length === 0
                    text: dialog.frozenTargets.length > 0
                        ? qsTr("%1 selected photos").arg(dialog.frozenTargets.length)
                        : qsTr("Select other photos in the gallery to link them. You can publish this node without adding new links.")
                    color: Theme.textSecondary
                    wrapMode: Text.WordWrap
                }
                ShadowCheckBox {
                    objectName: "sharedRelativePositionMasksOption"
                    Layout.fillWidth: true
                    visible: dialog.frozenTargets.length > 0
                        && (dialog.mode === "link" || dialog.includeSelectedPhotos)
                    text: qsTr("Include masks at relative positions")
                    checked: dialog.allowSpatialMasks
                    onToggled: dialog.allowSpatialMasks = checked
                }
                Label {
                    Layout.fillWidth: true
                    visible: dialog.frozenTargets.length > 0
                        && (dialog.mode === "link" || dialog.includeSelectedPhotos)
                    text: qsTr("Semantic masks are recalculated. Brushes and gradients require position copying; check their alignment on every new photo.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }
            }
        }

        Label {
            objectName: "sharedGradeLinkResultSummary"
            Layout.fillWidth: true
            visible: !dialog.inputMode
            text: dialog.available ? dialog.linkController.statusText : ""
            color: Theme.textPrimary
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: !dialog.inputMode && dialog.busy
            text: qsTr("You can close this panel and continue browsing. Stopping keeps updates that have already been saved.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: !dialog.inputMode && dialog.available
                && dialog.linkController.total > dialog.linkController.results.length
            text: qsTr("Recent results are shown below. Totals include earlier updates.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }
        ListView {
            id: resultList
            objectName: "sharedGradeLinkResultList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !dialog.inputMode
            clip: true
            spacing: 6
            model: dialog.available ? dialog.linkController.results : []
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: resultRow
                required property var modelData
                width: resultList.width
                implicitHeight: resultContent.implicitHeight + 20
                radius: Theme.compactControlRadius
                color: Theme.panel
                border.width: 1
                border.color: Theme.border
                ColumnLayout {
                    id: resultContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        text: String(resultRow.modelData.title || resultRow.modelData.photoId || "")
                        color: Theme.textPrimary
                        font.weight: Font.Medium
                        elide: Text.ElideMiddle
                    }
                    Label {
                        Layout.fillWidth: true
                        text: String(resultRow.modelData.message || "")
                        color: resultRow.modelData.state === "failed" ? Theme.warningText : Theme.textSecondary
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: String(resultRow.modelData.detail || "")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ShadowButton {
                objectName: "retrySharedGradeLinksButton"
                compact: true
                visible: !dialog.inputMode
                text: qsTr("Retry")
                enabled: dialog.available && dialog.linkController.canRetry
                onClicked: dialog.linkController.retryFailed()
            }
            ShadowButton {
                objectName: "stopSharedGradeLinksButton"
                compact: true
                visible: !dialog.inputMode && dialog.busy
                text: qsTr("Stop")
                onClicked: dialog.linkController.cancel()
            }
            Item { Layout.fillWidth: true }
            ShadowButton {
                objectName: "closeSharedGradeLinkDialogButton"
                compact: true
                variant: ShadowButton.Ghost
                text: dialog.inputMode ? qsTr("Cancel") : qsTr("Close")
                onClicked: dialog.close()
            }
            ShadowButton {
                objectName: "applySharedGradeLinksButton"
                compact: true
                variant: ShadowButton.Primary
                visible: dialog.inputMode
                text: dialog.mode === "publish" ? qsTr("Publish") : qsTr("Link")
                enabled: dialog.canApply
                onClicked: dialog.applyRequest()
            }
        }
    }
    footer: Item { implicitHeight: 0 }
}
