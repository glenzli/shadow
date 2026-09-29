pragma ComponentBehavior: Bound
pragma Translator: "ExportDialog"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Preset naming and removal are modal transactions. The settings pane supplies
// an immutable option snapshot or selected preset and receives only outcomes.
Item {
    id: menus

    required property var exportController
    property var pendingOptions: ({})
    property var pendingPreset: null
    property var pendingRemoval: null

    signal presetSaved(string presetId)
    signal presetRemoved()

    function openSave(options) {
        pendingOptions = options || {}
        pendingPreset = null
        presetNameField.text = ""
        presetNamePopup.open()
        presetNameField.forceActiveFocus()
    }

    function openEdit(preset, options) {
        if (preset === null || String(preset.id || "").startsWith("builtin-"))
            return
        pendingOptions = options || {}
        pendingPreset = preset
        presetNameField.text = String(preset.name || "")
        presetNamePopup.open()
        presetNameField.selectAll()
        presetNameField.forceActiveFocus()
    }

    function openRemove(preset) {
        if (preset === null || String(preset.id || "").startsWith("builtin-"))
            return
        pendingRemoval = preset
        presetRemovalPopup.open()
    }

    function removePendingPreset() {
        const preset = pendingRemoval
        presetRemovalPopup.close()
        pendingRemoval = null
        if (preset === null || String(preset.id || "").startsWith("builtin-"))
            return
        exportController.removePreset(String(preset.id))
        presetRemoved()
    }

    Popup {
        id: presetNamePopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: 340
        padding: 16
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.panelRaised
            radius: Theme.controlRadius
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                text: menus.pendingPreset === null
                      ? qsTr("SAVE EXPORT PRESET")
                      : qsTr("EDIT EXPORT PRESET")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
                font.letterSpacing: 0.8
            }

            ShadowTextField {
                id: presetNameField
                Layout.fillWidth: true
                placeholderText: qsTr("Preset name")
                color: Theme.textPrimary
                placeholderTextColor: Theme.textPlaceholder
                selectByMouse: true
                background: Rectangle {
                    color: Theme.control
                    radius: Theme.compactControlRadius
                    border.width: 1
                    border.color: presetNameField.activeFocus ? Theme.focusRing : Theme.border
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item {
                    Layout.fillWidth: true
                }
                ShadowButton {
                    text: qsTr("CANCEL")
                    variant: ShadowButton.Ghost
                    onClicked: presetNamePopup.close()
                }
                ShadowButton {
                    text: menus.pendingPreset === null ? qsTr("SAVE") : qsTr("UPDATE")
                    variant: ShadowButton.Primary
                    enabled: presetNameField.text.trim().length > 0
                    onClicked: {
                        const presetId = menus.pendingPreset === null
                            ? menus.exportController.savePreset(
                                  presetNameField.text, menus.pendingOptions)
                            : menus.exportController.updatePreset(
                                  String(menus.pendingPreset.id),
                                  presetNameField.text,
                                  menus.pendingOptions);
                        presetNamePopup.close();
                        menus.pendingPreset = null;
                        if (presetId.length > 0)
                            menus.presetSaved(presetId);
                    }
                }
            }
        }
    }

    Popup {
        id: presetRemovalPopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(390, parent.width - 40)
        padding: 16
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.panelRaised
            radius: Theme.controlRadius
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                text: qsTr("REMOVE PRESET")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
                font.letterSpacing: 0.8
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Remove “%1”?").arg(
                          String(menus.pendingRemoval
                                 ? menus.pendingRemoval.name : ""))
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Only this local preset will be removed. Exported files and other presets are unchanged.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Item { Layout.fillWidth: true }
                ShadowButton {
                    text: qsTr("CANCEL")
                    variant: ShadowButton.Ghost
                    onClicked: presetRemovalPopup.close()
                }
                ShadowButton {
                    text: qsTr("REMOVE")
                    variant: ShadowButton.Danger
                    onClicked: menus.removePendingPreset()
                }
            }
        }
    }

}
