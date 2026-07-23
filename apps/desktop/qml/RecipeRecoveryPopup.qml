pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    required property var editor
    required property color textPrimary
    required property color borderColor
    required property color panelRaised
    required property color errorBorder
    required property color errorText
    signal returnToReviewRequested()

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(456, parent.width - 48)
    padding: 0
    modal: true
    dim: true
    closePolicy: Popup.NoAutoClose
    visible: root.editor.recipeRecoveryRequired

    background: Rectangle {
        radius: Theme.controlRadius + 2
        color: root.panelRaised
        border.color: root.errorBorder
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 22
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("EDIT RECIPE NEEDS RESET")
                color: root.errorText
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 1.1
            }

            Label {
                Layout.fillWidth: true
                text: root.editor.recipeRecoveryErrorText
                color: root.textPrimary
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                lineHeight: 1.35
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: root.borderColor
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            spacing: 8

            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("RETURN TO REVIEW")
                variant: ShadowButton.Secondary
                enabled: !root.editor.stateBusy
                onClicked: root.returnToReviewRequested()
            }

            ShadowButton {
                Layout.fillWidth: true
                text: root.editor.stateBusy ? qsTr("RESETTING…") : qsTr("RESET EDITS")
                variant: ShadowButton.Danger
                enabled: !root.editor.stateBusy
                onClicked: root.editor.resetIncompatibleRecipe()
            }
        }
    }
}
