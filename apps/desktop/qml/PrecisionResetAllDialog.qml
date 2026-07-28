pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the destructive reset confirmation. The Inspector emits the intent;
// the editor remains the only authority that mutates and records the Recipe.
Popup {
    id: resetDialog

    required property var editor
    required property real hostWidth

    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(390, Math.max(0, resetDialog.hostWidth - 40))
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    padding: 18
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.dangerBorder
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: qsTr("Reset all adjustments?")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSection
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("This resets every Grade Node, mask, repair region, crop, and photo adjustment. You can undo it during this editing session.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
            lineHeight: 1.25
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            ShadowButton {
                compact: true
                text: qsTr("Cancel")
                onClicked: resetDialog.close()
            }

            ShadowButton {
                compact: true
                variant: ShadowButton.Danger
                text: qsTr("Reset all")
                enabled: resetDialog.editor.active
                    && !resetDialog.editor.stateBusy
                onClicked: {
                    resetDialog.editor.resetAllAdjustments()
                    resetDialog.close()
                }
            }
        }
    }
}
