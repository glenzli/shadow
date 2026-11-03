pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Presentation state only: the spatial-mask choice persists for this session.
// Closing the results view neither cancels the batch nor discards its receipt.
ShadowDialog {
    id: dialog
    objectName: "adjustmentTransferDialog"

    property var transferController: null
    property var editor: null
    property bool copyMode: true
    property bool allowSpatialMasks: false
    readonly property bool available: transferController !== null
    readonly property bool admissionEnabled: available
        && transferController.admissionEnabled !== false
    readonly property bool busy: available
        && (transferController.busy || transferController.copying)
    readonly property var copyNodes: available ? transferController.copyOptions : []
    property var chosenKeys: ({})
    property string optionsStamp: ""
    readonly property var choices: {
        const values = []
        for (let index = 0; index < copyNodes.length; ++index) {
            const node = copyNodes[index]
            const keys = Object.keys(chosenKeys[String(node.nodeId)] || {})
                .filter(key => chosenKeys[String(node.nodeId)][key])
            if (keys.length > 0)
                values.push({"nodeId": String(node.nodeId), "keys": keys})
        }
        return values
    }
    readonly property bool copyingMasks: choices.some(choice => choice.keys.indexOf("mask") >= 0)
    onCopyNodesChanged: {
        const stamp = JSON.stringify(copyNodes)
        if (stamp === optionsStamp)
            return
        optionsStamp = stamp
        chooseAll(true)
    }

    function chooseAll(enabled) {
        const next = ({})
        for (let index = 0; index < copyNodes.length; ++index) {
            const node = copyNodes[index]
            next[String(node.nodeId)] = ({})
            for (let item = 0; item < node.items.length; ++item)
                next[String(node.nodeId)][String(node.items[item].key)] = enabled
        }
        chosenKeys = next
    }

    function chooseNode(nodeId, enabled) {
        const next = Object.assign({}, chosenKeys)
        const keys = ({})
        for (let index = 0; index < copyNodes.length; ++index) {
            const node = copyNodes[index]
            if (String(node.nodeId) !== nodeId)
                continue
            for (let item = 0; item < node.items.length; ++item)
                keys[String(node.items[item].key)] = enabled
        }
        next[nodeId] = keys
        chosenKeys = next
    }

    function chooseItem(nodeId, key, enabled) {
        const next = Object.assign({}, chosenKeys)
        next[nodeId] = Object.assign({}, next[nodeId] || {})
        next[nodeId][key] = enabled
        chosenKeys = next
    }

    function selectedItemCount(nodeId) {
        const keys = chosenKeys[nodeId] || {}
        return Object.keys(keys).filter(key => keys[key]).length
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(copyMode ? 520 : 680, Math.max(0, (parent ? parent.width : 728) - 48))
    height: Math.min(copyMode ? 500 : 580, Math.max(0, (parent ? parent.height : 628) - 48))
    modal: copyMode
    palette.base: Theme.controlQuiet
    palette.light: Theme.controlPressed
    palette.mid: Theme.borderStrong
    palette.highlight: Theme.accent
    palette.text: Theme.accent
    title: copyMode ? qsTr("Copy style") : qsTr("Adjustment transfer results")
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function presentCopy(target) {
        if (!admissionEnabled || busy)
            return
        copyMode = true
        optionsStamp = ""
        if (transferController.prepareCopy(target || {}))
            open()
    }

    function presentResults() {
        if (!available)
            return
        copyMode = false
        open()
    }

    function resultStateLabel(state) {
        switch (String(state)) {
        case "pending": return qsTr("Waiting")
        case "running": return qsTr("Applying")
        case "applied": return qsTr("Applied")
        case "unchanged": return qsTr("Unchanged")
        case "failed": return qsTr("Failed")
        case "cancelled": return qsTr("Cancelled")
        case "undone": return qsTr("Undone")
        default: return qsTr("Waiting")
        }
    }

    contentItem: ColumnLayout {
        spacing: 12

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.copyMode
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: 10

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Copied nodes are appended to each destination photo. Existing edits are preserved.")
                    color: Theme.textPrimary
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    text: dialog.available ? dialog.transferController.copySourceName : ""
                    color: Theme.textSecondary
                    elide: Text.ElideMiddle
                    font.weight: Font.DemiBold
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Only modified settings are shown")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                    }
                    ShadowButton {
                        compact: true
                        variant: ShadowButton.Ghost
                        text: qsTr("Select all")
                        onClicked: dialog.chooseAll(true)
                    }
                    ShadowButton {
                        compact: true
                        variant: ShadowButton.Ghost
                        text: qsTr("Select none")
                        onClicked: dialog.chooseAll(false)
                    }
                }

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    visible: dialog.available && dialog.transferController.preparingCopy
                    running: visible
                }

                Repeater {
                    model: dialog.copyNodes
                    delegate: Rectangle {
                        id: nodeChoice
                        required property var modelData
                        property bool expanded: false
                        readonly property string nodeId: String(modelData.nodeId)
                        Layout.fillWidth: true
                        implicitHeight: nodeContents.implicitHeight + 16
                        radius: Theme.controlRadius
                        color: Theme.panel
                        border.color: Theme.border
                        ColumnLayout {
                            id: nodeContents
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 8
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                ShadowCheckBox {
                                    objectName: "copyNode_" + nodeChoice.nodeId
                                    Layout.fillWidth: true
                                    text: String(nodeChoice.modelData.label)
                                    tristate: true
                                    checkState: dialog.selectedItemCount(nodeChoice.nodeId) === 0
                                        ? Qt.Unchecked
                                        : dialog.selectedItemCount(nodeChoice.nodeId)
                                            === nodeChoice.modelData.items.length
                                            ? Qt.Checked : Qt.PartiallyChecked
                                    onClicked: dialog.chooseNode(nodeChoice.nodeId,
                                        dialog.selectedItemCount(nodeChoice.nodeId)
                                            !== nodeChoice.modelData.items.length)
                                }
                                ShadowIconButton {
                                    objectName: "expandCopyNode_" + nodeChoice.nodeId
                                    source: "qrc:/icons/chevron-down.svg"
                                    rotation: nodeChoice.expanded ? 180 : 0
                                    toolTipText: nodeChoice.expanded
                                        ? qsTr("Hide settings") : qsTr("Choose settings")
                                    onClicked: nodeChoice.expanded = !nodeChoice.expanded
                                }
                                Label {
                                    text: dialog.selectedItemCount(nodeChoice.nodeId)
                                        + " / " + nodeChoice.modelData.items.length
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontMeta
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                visible: nodeChoice.expanded
                                Repeater {
                                    model: nodeChoice.modelData.items
                                    delegate: ShadowCheckBox {
                                        required property var modelData
                                        objectName: "copyItem_" + nodeChoice.nodeId + "_" + modelData.key
                                        Layout.fillWidth: true
                                        compact: true
                                        text: String(modelData.label)
                                        checked: Boolean((dialog.chosenKeys[nodeChoice.nodeId] || {})[String(modelData.key)])
                                        onClicked: dialog.chooseItem(nodeChoice.nodeId,
                                            String(modelData.key), checked)
                                    }
                                }
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: dialog.available && !dialog.transferController.preparingCopy
                        && dialog.copyNodes.length === 0
                    text: dialog.available && dialog.transferController.copyError.length > 0
                        ? dialog.transferController.copyError
                        : qsTr("This photo has no modified adjustment settings to copy.")
                    wrapMode: Text.WordWrap
                    color: Theme.textSecondary
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("RAW development, crop, repair, and other structural edits are not copied.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }

                Rectangle {
                    visible: dialog.copyingMasks
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.border
                }

                Label {
                    visible: dialog.copyingMasks
                    Layout.fillWidth: true
                    text: qsTr("Semantic masks are recalculated for each destination photo. If a mask cannot be recalculated, that photo is reported without changing its edits.")
                    color: Theme.textSecondary
                    wrapMode: Text.WordWrap
                }

                ShadowCheckBox {
                    objectName: "copyRelativePositionMasksOption"
                    visible: dialog.copyingMasks
                    Layout.fillWidth: true
                    text: qsTr("Include masks at their relative positions")
                    checked: dialog.allowSpatialMasks
                    onClicked: dialog.allowSpatialMasks = checked
                }

                Label {
                    visible: dialog.copyingMasks
                    Layout.fillWidth: true
                    text: qsTr("Brushes and gradients keep their relative positions. Check their alignment on each destination photo.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !dialog.copyMode && text.length > 0
            text: dialog.available && dialog.transferController.batchSourceName
                ? qsTr("Adjustments from %1").arg(dialog.transferController.batchSourceName) : ""
            color: Theme.textSecondary
            elide: Text.ElideRight
        }

        Label {
            Layout.fillWidth: true
            visible: !dialog.copyMode
            text: dialog.available ? dialog.busy ? dialog.transferController.statusText
                : qsTr("%1 completed · %2 need attention")
                    .arg(dialog.transferController.completed).arg(dialog.transferController.failed) : ""
            color: Theme.textPrimary
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            visible: !dialog.copyMode && dialog.busy
            text: qsTr("You can close this panel and continue browsing while adjustments are applied.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        ListView {
            id: resultList
            objectName: "adjustmentTransferResultList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !dialog.copyMode
            clip: true
            spacing: 6
            model: dialog.available ? dialog.transferController.results : []
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: resultRow
                required property var modelData
                property bool detailExpanded: false
                readonly property string detailText: String(modelData.detail || "")
                width: resultList.width
                implicitHeight: resultContent.implicitHeight + 20
                radius: Theme.compactControlRadius
                color: Theme.panel
                border.color: Theme.border

                ColumnLayout {
                    id: resultContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 5

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Label {
                            Layout.fillWidth: true
                            text: String(resultRow.modelData.title
                                || resultRow.modelData.sourcePath || resultRow.modelData.photoId)
                            color: Theme.textPrimary
                            font.weight: Font.Medium
                            elide: Text.ElideMiddle
                        }

                        Label {
                            text: dialog.resultStateLabel(resultRow.modelData.state)
                            color: String(resultRow.modelData.state) === "failed"
                                ? Theme.errorText : Theme.textSecondary
                            font.pixelSize: Theme.fontMeta
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: String(resultRow.modelData.message || "")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }

                    ShadowButton {
                        objectName: "adjustmentTransferDetailButton"
                        visible: resultRow.detailText.length > 0
                        compact: true
                        variant: ShadowButton.Ghost
                        text: resultRow.detailExpanded ? qsTr("Hide details") : qsTr("Show details")
                        onClicked: resultRow.detailExpanded = !resultRow.detailExpanded
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: resultRow.detailExpanded && resultRow.detailText.length > 0
                        text: resultRow.detailText
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: resultList.count === 0
                text: qsTr("No transfer results yet")
                color: Theme.textMuted
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ShadowButton {
                objectName: "retryAdjustmentTransferButton"
                visible: !dialog.copyMode && dialog.available && dialog.transferController.canRetry
                text: qsTr("Retry failed")
                enabled: !dialog.busy
                onClicked: dialog.transferController.retryFailed()
            }

            ShadowButton {
                objectName: "undoAdjustmentTransferButton"
                visible: !dialog.copyMode && dialog.available && dialog.transferController.canUndo
                text: qsTr("Undo this batch")
                enabled: !dialog.busy
                onClicked: dialog.transferController.undoBatch()
            }

            Item { Layout.fillWidth: true }

            ShadowButton {
                objectName: "closeAdjustmentTransferDialogButton"
                text: dialog.copyMode ? qsTr("Cancel") : qsTr("Close")
                onClicked: dialog.close()
            }

            ShadowButton {
                objectName: "confirmCopyAdjustmentsButton"
                visible: dialog.copyMode
                text: qsTr("Copy")
                variant: ShadowButton.Primary
                enabled: dialog.admissionEnabled && !dialog.busy
                    && dialog.choices.length > 0
                onClicked: {
                    dialog.transferController.copyChosenAdjustments(
                        dialog.choices, dialog.allowSpatialMasks)
                    dialog.close()
                }
            }
        }
    }
}
