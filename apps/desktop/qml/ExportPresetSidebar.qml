pragma ComponentBehavior: Bound
pragma Translator: "ExportDialog"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns preset-library selection and preset transactions. The settings pane
// remains the owner of the live export draft applied by each selection.
Item {
    id: sidebar

    required property var exportController
    required property var optionProvider
    property string currentPresetId: ""

    signal presetActivated(var preset)

    function presetAt(index) {
        const presets = exportController.presets || []
        return index >= 0 && index < presets.length ? presets[index] : null
    }

    function selectedPreset() {
        const presets = exportController.presets || []
        for (let index = 0; index < presets.length; ++index) {
            if (String(presets[index].id || "") === currentPresetId)
                return presets[index]
        }
        return null
    }

    function selectedPresetIsCustom() {
        const preset = selectedPreset()
        return preset !== null && !String(preset.id || "").startsWith("builtin-")
    }

    function selectPresetId(presetId) {
        const presets = exportController.presets || []
        let selected = null
        for (let index = 0; index < presets.length; ++index) {
            if (String(presets[index].id || "") === String(presetId)) {
                selected = presets[index]
                break
            }
        }
        if (selected === null && presets.length > 0)
            selected = presets[0]
        currentPresetId = selected ? String(selected.id || "") : ""
        if (selected)
            presetActivated(selected)
    }

    function resetSelection() {
        selectPresetId("")
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.panel

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 12
                Layout.topMargin: 16
                Layout.bottomMargin: 10
                text: qsTr("EXPORT PRESETS")
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                font.letterSpacing: 0.7
            }

            ListView {
                id: presetList
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                clip: true
                spacing: 3
                model: sidebar.exportController.presets

                delegate: Rectangle {
                    id: presetDelegate
                    required property var modelData
                    width: presetList.width
                    height: 54
                    radius: Theme.compactControlRadius
                    color: sidebar.currentPresetId === String(modelData.id || "")
                           ? Theme.accentSurface
                           : presetMouse.containsMouse ? Theme.controlQuiet : "transparent"
                    border.width: sidebar.currentPresetId === String(modelData.id || "") ? 1 : 0
                    border.color: Theme.accentBorder

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 10
                        anchors.rightMargin: 8
                        spacing: 3

                        Label {
                            width: parent.width
                            text: String(presetDelegate.modelData.name || "")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSection
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Label {
                            width: parent.width
                            text: {
                                const format = String(presetDelegate.modelData.format || "jpeg").toUpperCase()
                                const edge = Number(presetDelegate.modelData.maxEdge || 0)
                                return edge > 0
                                    ? qsTr("%1 · %L2 px").arg(format).arg(edge)
                                    : qsTr("%1 · original size").arg(format)
                            }
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: presetMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: sidebar.selectPresetId(String(presetDelegate.modelData.id || ""))
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10
                Layout.topMargin: 8
                Layout.bottomMargin: 8
                spacing: 4

                ShadowIconButton {
                    source: "qrc:/icons/node-add.svg"
                    toolTipText: qsTr("Save current settings as a preset")
                    accessibleName: toolTipText
                    enabled: !sidebar.exportController.busy
                    onClicked: presetMenus.openSave(sidebar.optionProvider())
                }
                ShadowIconButton {
                    source: "qrc:/icons/edit.svg"
                    toolTipText: qsTr("Update selected preset")
                    accessibleName: toolTipText
                    enabled: !sidebar.exportController.busy && sidebar.selectedPresetIsCustom()
                    onClicked: presetMenus.openEdit(
                                   sidebar.selectedPreset(), sidebar.optionProvider())
                }
                ShadowIconButton {
                    source: "qrc:/icons/trash.svg"
                    toolTipText: qsTr("Remove selected export preset")
                    accessibleName: toolTipText
                    enabled: !sidebar.exportController.busy && sidebar.selectedPresetIsCustom()
                    onClicked: presetMenus.openRemove(sidebar.selectedPreset())
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    ExportPresetMenus {
        id: presetMenus
        exportController: sidebar.exportController
        onPresetSaved: presetId => sidebar.selectPresetId(presetId)
        onPresetRemoved: sidebar.resetSelection()
    }
}
