pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Popups share the selected Grade Node transaction and remain independent of
// the pane layout. Callers provide only the anchor and the intended target.
Item {
    id: menus

    required property var editor
    required property var interchangeController
    property var lutExportController: null
    property var adjustmentTransferController: null
    property var sharedGradeLinkController: null
    readonly property var selectedSharedState: gradeNodeContextPopup.targetData
        || editor.selectedSharedGradeNodeState || ({})

    function openExchange(anchorItem) {
        const point = anchorItem.mapToItem(exchangePopup.parent, 0, anchorItem.height + 5)
        exchangePopup.x = point.x
        exchangePopup.y = point.y
        exchangePopup.open()
    }

    signal cropGeometryRequested
    signal repairRequested
    signal liquifyRequested

    function openAdd(anchorItem) {
        const point = anchorItem.mapToItem(
            addGradeNodePopup.parent,
            anchorItem.width - addGradeNodePopup.width,
            anchorItem.height + 5)
        addGradeNodePopup.x = point.x
        addGradeNodePopup.y = point.y
        addGradeNodePopup.open()
    }

    function openContext(anchorItem, pointerY, targetData) {
        gradeNodeContextPopup.targetData = targetData
        const point = anchorItem.mapToItem(
            gradeNodeContextPopup.parent,
            anchorItem.width - gradeNodeContextPopup.width,
            pointerY)
        gradeNodeContextPopup.x = point.x
        gradeNodeContextPopup.y = point.y
        gradeNodeContextPopup.open()
    }

    component PopupAction: Button {
        id: popupAction
        width: parent ? parent.width : 204
        implicitHeight: 32
        leftPadding: 10
        rightPadding: 10
        hoverEnabled: enabled
        background: Rectangle {
            radius: 5
            color: !popupAction.enabled ? Theme.transparent
                : popupAction.down ? Theme.buttonGhostPressed
                : popupAction.hovered ? Theme.buttonGhostHover
                : Theme.transparent
        }
        contentItem: Label {
            text: popupAction.text
            color: popupAction.enabled ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: Theme.fontSection
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    Popup {
        id: addGradeNodePopup
        parent: Overlay.overlay
        width: 224
        padding: 7
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: menus.editor.refreshSharedGradeNodes()
        background: Rectangle {
            radius: 8
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
        contentItem: Column {
            spacing: 2

            Label {
                width: parent.width
                leftPadding: 10
                topPadding: 6
                bottomPadding: 3
                text: qsTr("STRUCTURAL")
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }

            PopupAction {
                text: menus.editor.canvasNodeMaterialized
                    ? qsTr("Open Crop & Geometry")
                    : qsTr("Add Crop & Geometry")
                onClicked: {
                    addGradeNodePopup.close()
                    menus.editor.addCanvasNode()
                    menus.editor.selectCanvasNode()
                    menus.cropGeometryRequested()
                }
            }

            PopupAction {
                text: menus.editor.rawDenoiseNodeMaterialized
                    ? qsTr("Open AI RAW Denoise")
                    : qsTr("Add AI RAW Denoise")
                onClicked: {
                    addGradeNodePopup.close()
                    menus.editor.addRawDenoiseNode()
                    menus.editor.selectRawDenoiseNode()
                }
            }

            PopupAction {
                text: menus.editor.liquifyNodeMaterialized
                    ? qsTr("Open Liquify")
                    : qsTr("Add Liquify")
                onClicked: {
                    addGradeNodePopup.close()
                    menus.editor.selectLiquifyNode()
                    menus.liquifyRequested()
                }
            }

            PopupAction {
                text: qsTr("Repair")
                onClicked: {
                    addGradeNodePopup.close()
                    menus.editor.selectRetouchNode()
                    menus.repairRequested()
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.border
            }

            Label {
                width: parent.width
                leftPadding: 10
                topPadding: 6
                bottomPadding: 3
                text: qsTr("ADJUSTMENTS")
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }

            PopupAction {
                text: qsTr("New adjustment")
                enabled: menus.editor.canAddGradeNode
                onClicked: {
                    addGradeNodePopup.close()
                    menus.editor.addGradeNode()
                }
            }

            Label {
                width: parent.width
                leftPadding: 10
                topPadding: 6
                bottomPadding: 3
                text: qsTr("SHARED NODES")
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }

            Label {
                visible: menus.editor.sharedGradeNodes.length === 0
                width: parent.width
                leftPadding: 10
                rightPadding: 10
                topPadding: 6
                bottomPadding: 8
                text: qsTr("No shared nodes yet")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }

            Repeater {
                model: menus.editor.sharedGradeNodes
                delegate: PopupAction {
                    required property var modelData
                    text: modelData.label
                    enabled: menus.editor.canAddGradeNode
                    onClicked: {
                        addGradeNodePopup.close()
                        menus.editor.insertSharedGradeNode(modelData.layerId)
                    }
                }
            }
        }
    }

    Popup {
        id: gradeNodeContextPopup
        objectName: "gradeNodeContextPopup"
        parent: Overlay.overlay
        width: 224
        padding: 7
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        property var targetData: null
        onOpened: {
            x = Math.max(8, Math.min(x, parent.width - width - 8))
            y = Math.max(8, Math.min(y, parent.height - height - 8))
        }
        background: Rectangle {
            radius: 8
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
        contentItem: Column {
            spacing: 2
            PopupAction {
                objectName: "copyAdjustmentNodeAction"
                visible: menus.adjustmentTransferController !== null
                text: qsTr("Copy node")
                enabled: menus.adjustmentTransferController !== null
                    && !menus.adjustmentTransferController.busy
                    && !menus.adjustmentTransferController.copying
                    && menus.editor.active && menus.editor.hasSelectedGradeNode
                    && !menus.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.adjustmentTransferController.copyAdjustments(true, false)
                }
            }
            PopupAction {
                objectName: "publishSharedNodeAction"
                visible: menus.sharedGradeLinkController !== null
                text: gradeNodeContextPopup.targetData
                    && gradeNodeContextPopup.targetData.shared
                    ? qsTr("Publish update…") : qsTr("Share and link…")
                enabled: menus.sharedGradeLinkController !== null
                    && menus.sharedGradeLinkController.admissionEnabled
                    && !menus.sharedGradeLinkController.busy
                    && menus.editor.active && !menus.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.sharedGradeLinkController.requestPublication()
                }
            }
            PopupAction {
                objectName: "linkSharedNodeAction"
                visible: menus.sharedGradeLinkController !== null
                    && gradeNodeContextPopup.targetData && gradeNodeContextPopup.targetData.shared
                text: qsTr("Link to selected photos…")
                enabled: menus.sharedGradeLinkController !== null
                    && menus.sharedGradeLinkController.admissionEnabled
                    && !menus.sharedGradeLinkController.busy
                    && menus.editor.active && !menus.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.sharedGradeLinkController.requestLinks()
                }
            }
            PopupAction {
                objectName: "syncSharedNodeAction"
                visible: menus.sharedGradeLinkController !== null
                    && gradeNodeContextPopup.targetData && gradeNodeContextPopup.targetData.shared
                    && menus.selectedSharedState.sharedFollowHead === true
                text: qsTr("Update to latest")
                enabled: menus.sharedGradeLinkController !== null
                    && menus.sharedGradeLinkController.admissionEnabled
                    && !menus.sharedGradeLinkController.busy
                    && menus.editor.active && !menus.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.sharedGradeLinkController.syncSelected()
                }
            }
            PopupAction {
                objectName: "freezeSharedNodeAction"
                visible: gradeNodeContextPopup.targetData && gradeNodeContextPopup.targetData.shared
                text: menus.selectedSharedState.sharedFollowHead === true
                    ? qsTr("Keep this revision") : qsTr("Follow published updates")
                enabled: menus.editor.active && !menus.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.editor.setSelectedGradeNodeSharedFollowHead(
                        menus.selectedSharedState.sharedFollowHead !== true)
                }
            }
            PopupAction {
                objectName: "unlinkSharedNodeAction"
                visible: gradeNodeContextPopup.targetData && gradeNodeContextPopup.targetData.shared
                text: qsTr("Unlink node")
                enabled: menus.editor.active && !menus.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.editor.unlinkSelectedGradeNode()
                }
            }
            PopupAction {
                text: qsTr("Duplicate as independent")
                enabled: menus.editor.canAddGradeNode
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.editor.duplicateSelectedGradeNode()
                }
            }
            PopupAction {
                text: qsTr("Reset node")
                enabled: menus.editor.active
                    && menus.editor.hasSelectedGradeNode
                    && !menus.editor.stateBusy
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.editor.resetSelectedGradeNode()
                }
            }
            PopupAction {
                text: qsTr("Delete node")
                enabled: menus.editor.canDeleteGradeNode
                onClicked: {
                    gradeNodeContextPopup.close()
                    menus.editor.deleteSelectedGradeNode()
                }
            }
        }
    }

    Popup {
        id: exchangePopup
        parent: Overlay.overlay
        width: 280
        padding: 7
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            radius: 8
            color: Theme.panelRaised
            border.color: Theme.borderStrong
        }
        contentItem: Column {
            spacing: 2
            PopupAction {
                text: qsTr("Import Shadow Recipe…")
                onClicked: { exchangePopup.close(); shadowRecipeDialog.chooseImportFile() }
            }
            PopupAction {
                text: qsTr("Import XMP adjustments…")
                enabled: menus.editor.canAddGradeNode
                onClicked: { exchangePopup.close(); xmpImportDialog.chooseFile() }
            }
            PopupAction {
                text: qsTr("Export current Recipe…")
                onClicked: { exchangePopup.close(); shadowRecipeDialog.chooseExportFile(false) }
            }
            PopupAction {
                text: qsTr("Export selected Grade Node…")
                enabled: menus.editor.selectedRecipeNodeKind === "grade"
                onClicked: { exchangePopup.close(); shadowRecipeDialog.chooseExportFile(true) }
            }
            PopupAction {
                text: qsTr("Export grading as LUT…")
                enabled: menus.lutExportController !== null
                onClicked: { exchangePopup.close(); lutExportDialog.present(false) }
            }
            PopupAction {
                text: qsTr("Export selected node as LUT…")
                enabled: menus.lutExportController !== null
                    && menus.editor.selectedRecipeNodeKind === "grade"
                onClicked: { exchangePopup.close(); lutExportDialog.present(true) }
            }
        }
    }

    LutExportDialog {
        id: lutExportDialog
        controller: menus.lutExportController
    }

    XmpImportDialog {
        id: xmpImportDialog
        interchangeController: menus.interchangeController
    }

    ShadowRecipeInterchangeDialog {
        id: shadowRecipeDialog
        editor: menus.editor
        interchangeController: menus.interchangeController
    }
}
