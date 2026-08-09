pragma ComponentBehavior: Bound
pragma Translator: "ExportDialog"

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Owns the reusable local watermark-definition library and the watermark
// snapshot attached to the current output recipe.
ColumnLayout {
    id: pane

    required property var exportController
    property string selectedDefinitionId: ""
    property string watermarkPath: ""
    property real watermarkOpacity: 0.72
    property real watermarkScale: 0.18
    property real watermarkInset: 0.02
    property string watermarkAnchor: "bottom-right"
    property bool managerExpanded: false

    spacing: 9

    function definitions() {
        return exportController.watermarks || []
    }

    function definitionAt(index) {
        const definitions = pane.definitions()
        const adjusted = index - 1
        return adjusted >= 0 && adjusted < definitions.length
            ? definitions[adjusted] : null
    }

    function definitionById(definitionId) {
        const definitions = pane.definitions()
        for (let index = 0; index < definitions.length; ++index) {
            if (String(definitions[index].id || "") === String(definitionId))
                return definitions[index]
        }
        return null
    }

    function definitionIndex(definitionId) {
        const definitions = pane.definitions()
        for (let index = 0; index < definitions.length; ++index) {
            if (String(definitions[index].id || "") === String(definitionId))
                return index + 1
        }
        return 0
    }

    function applyDefinition(definition) {
        if (!definition) {
            selectedDefinitionId = ""
            watermarkPath = ""
            return
        }
        selectedDefinitionId = String(definition.id || "")
        applySnapshot(definition, true)
        definitionNameField.text = String(definition.name || "")
    }

    function applySnapshot(snapshot, keepDefinitionId) {
        if (!snapshot)
            return
        if (!keepDefinitionId)
            selectedDefinitionId = ""
        watermarkPath = String(snapshot.watermarkPath || "")
        watermarkOpacity = Number(snapshot.watermarkOpacity !== undefined
                                  ? snapshot.watermarkOpacity : 0.72)
        watermarkScale = Number(snapshot.watermarkScale !== undefined
                                ? snapshot.watermarkScale : 0.18)
        watermarkInset = Number(snapshot.watermarkInset !== undefined
                                ? snapshot.watermarkInset : 0.02)
        watermarkAnchor = String(snapshot.watermarkAnchor || "bottom-right")
    }

    function snapshot() {
        return {
            "watermarkPath": String(watermarkPath),
            "watermarkOpacity": Number(watermarkOpacity),
            "watermarkScale": Number(watermarkScale),
            "watermarkInset": Number(watermarkInset),
            "watermarkAnchor": String(watermarkAnchor)
        }
    }

    function selectDefinitionId(definitionId) {
        const definition = definitionById(definitionId)
        applyDefinition(definition)
    }

    Label {
        text: qsTr("WATERMARK")
        color: Theme.textMuted
        font.pixelSize: 9
        font.weight: Font.DemiBold
        font.letterSpacing: 0.7
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 7

        ComboBox {
            id: definitionBox
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeight
            model: [{"id": "", "name": qsTr("No watermark")}].concat(
                       pane.exportController.watermarks || [])
            textRole: "name"
            currentIndex: pane.definitionIndex(pane.selectedDefinitionId)
            onActivated: pane.applyDefinition(pane.definitionAt(currentIndex))
            contentItem: Label {
                leftPadding: 10
                rightPadding: 28
                verticalAlignment: Text.AlignVCenter
                text: definitionBox.displayText
                color: Theme.textPrimary
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            background: Rectangle {
                color: Theme.control
                radius: Theme.compactControlRadius
                border.width: 1
                border.color: definitionBox.activeFocus ? Theme.focusRing : Theme.border
            }
        }

        ShadowButton {
            text: pane.managerExpanded ? qsTr("DONE") : qsTr("MANAGE")
            variant: ShadowButton.Secondary
            enabled: !pane.exportController.busy
            onClicked: pane.managerExpanded = !pane.managerExpanded
        }
    }

    Label {
        Layout.fillWidth: true
        visible: !pane.managerExpanded && pane.watermarkPath.length > 0
        text: pane.watermarkPath.split("/").pop()
        color: Theme.textMuted
        font.pixelSize: 9
        elide: Text.ElideMiddle
    }

    Rectangle {
        Layout.fillWidth: true
        visible: pane.managerExpanded
        implicitHeight: managerLayout.implicitHeight + 20
        radius: Theme.controlRadius
        color: Theme.controlQuiet
        border.width: 1
        border.color: Theme.border

        ColumnLayout {
            id: managerLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 10
            spacing: 9

            TextField {
                id: definitionNameField
                Layout.fillWidth: true
                placeholderText: qsTr("Watermark name")
                color: Theme.textPrimary
                placeholderTextColor: Theme.textPlaceholder
                selectByMouse: true
                background: Rectangle {
                    color: Theme.control
                    radius: Theme.compactControlRadius
                    border.width: 1
                    border.color: definitionNameField.activeFocus
                                  ? Theme.focusRing : Theme.border
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 7
                ShadowButton {
                    Layout.fillWidth: true
                    text: pane.watermarkPath.length > 0
                          ? pane.watermarkPath.split("/").pop()
                          : qsTr("CHOOSE PNG")
                    variant: ShadowButton.Secondary
                    onClicked: watermarkFileDialog.open()
                }
                ShadowIconButton {
                    source: "qrc:/icons/clear.svg"
                    toolTipText: qsTr("Remove watermark image")
                    accessibleName: toolTipText
                    enabled: pane.watermarkPath.length > 0
                    onClicked: pane.watermarkPath = ""
                }
            }

            GridLayout {
                Layout.alignment: Qt.AlignHCenter
                columns: 3
                rowSpacing: 4
                columnSpacing: 4
                Repeater {
                    model: ["top-left", "top-center", "top-right",
                            "middle-left", "middle-center", "middle-right",
                            "bottom-left", "bottom-center", "bottom-right"]
                    delegate: Rectangle {
                        id: anchorCell
                        required property string modelData
                        width: 34
                        height: 26
                        radius: Theme.compactControlRadius
                        color: pane.watermarkAnchor === modelData
                               ? Theme.accentSurface : Theme.control
                        border.width: pane.watermarkAnchor === modelData ? 1 : 0
                        border.color: Theme.accentBorder
                        Rectangle {
                            anchors.centerIn: parent
                            width: 5
                            height: 5
                            radius: width / 2
                            color: pane.watermarkAnchor === anchorCell.modelData
                                   ? Theme.accent : Theme.textMuted
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: pane.watermarkAnchor = anchorCell.modelData
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 68
                    text: qsTr("Opacity")
                    color: Theme.textSecondary
                    font.pixelSize: 10
                }
                ShadowInlineSlider {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    neutralValue: 0.72
                    fillFromMinimum: true
                    stepSize: 0.01
                    value: pane.watermarkOpacity
                    onMoved: pane.watermarkOpacity = value
                    onResetRequested: value => pane.watermarkOpacity = value
                }
                Label {
                    Layout.preferredWidth: 34
                    text: qsTr("%1%").arg(Math.round(pane.watermarkOpacity * 100))
                    color: Theme.textMuted
                    font.pixelSize: 9
                    horizontalAlignment: Text.AlignRight
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 68
                    text: qsTr("Scale")
                    color: Theme.textSecondary
                    font.pixelSize: 10
                }
                ShadowInlineSlider {
                    Layout.fillWidth: true
                    from: 0.03
                    to: 0.5
                    neutralValue: 0.18
                    fillFromMinimum: true
                    stepSize: 0.01
                    value: pane.watermarkScale
                    onMoved: pane.watermarkScale = value
                    onResetRequested: value => pane.watermarkScale = value
                }
                Label {
                    Layout.preferredWidth: 34
                    text: qsTr("%1%").arg(Math.round(pane.watermarkScale * 100))
                    color: Theme.textMuted
                    font.pixelSize: 9
                    horizontalAlignment: Text.AlignRight
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.preferredWidth: 68
                    text: qsTr("Inset")
                    color: Theme.textSecondary
                    font.pixelSize: 10
                }
                ShadowInlineSlider {
                    Layout.fillWidth: true
                    from: 0
                    to: 0.25
                    neutralValue: 0.02
                    fillFromMinimum: true
                    stepSize: 0.005
                    value: pane.watermarkInset
                    onMoved: pane.watermarkInset = value
                    onResetRequested: value => pane.watermarkInset = value
                }
                Label {
                    Layout.preferredWidth: 34
                    text: qsTr("%1%").arg(Math.round(pane.watermarkInset * 100))
                    color: Theme.textMuted
                    font.pixelSize: 9
                    horizontalAlignment: Text.AlignRight
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ShadowButton {
                    text: qsTr("DELETE")
                    variant: ShadowButton.Danger
                    enabled: pane.selectedDefinitionId.length > 0
                    onClicked: {
                        pane.exportController.removeWatermark(pane.selectedDefinitionId)
                        pane.applyDefinition(null)
                        definitionNameField.text = ""
                    }
                }
                ShadowButton {
                    text: pane.selectedDefinitionId.length > 0
                          ? qsTr("UPDATE") : qsTr("SAVE")
                    variant: ShadowButton.Primary
                    enabled: definitionNameField.text.trim().length > 0
                             && pane.watermarkPath.length > 0
                    onClicked: {
                        const id = pane.selectedDefinitionId.length > 0
                            ? pane.exportController.updateWatermark(
                                  pane.selectedDefinitionId,
                                  definitionNameField.text,
                                  pane.snapshot())
                            : pane.exportController.saveWatermark(
                                  definitionNameField.text,
                                  pane.snapshot())
                        if (id.length > 0)
                            pane.selectDefinitionId(id)
                    }
                }
            }
        }
    }

    FileDialog {
        id: watermarkFileDialog
        title: qsTr("Choose a PNG watermark")
        nameFilters: [qsTr("PNG images (*.png)")]
        onAccepted: pane.watermarkPath = selectedFile.toString()
    }
}
