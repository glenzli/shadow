pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Presentation only. Each caller keeps its own acceptance and cancellation policy.
Dialog {
    id: dialog

    parent: Overlay.overlay
    popupType: Popup.Item
    modal: true
    focus: true
    padding: 20
    topPadding: 0
    bottomPadding: footer && footer.visible ? 0 : 20
    spacing: 16
    font.pixelSize: Theme.fontBody
    palette.windowText: Theme.textPrimary
    palette.text: Theme.textPrimary
    palette.buttonText: Theme.textPrimary

    // Qt's standard actions otherwise depend on a separate Qt translation
    // catalog. Keep the actions used by Shadow in the product catalog instead.
    function localizeStandardActions() {
        const acceptButton = standardButton(Dialog.Ok)
        if (acceptButton)
            acceptButton.text = Qt.binding(function() { return qsTr("OK") })
        const cancelButton = standardButton(Dialog.Cancel)
        if (cancelButton)
            cancelButton.text = Qt.binding(function() { return qsTr("Cancel") })
        const saveButton = standardButton(Dialog.Save)
        if (saveButton)
            saveButton.text = Qt.binding(function() { return qsTr("Save") })
    }
    onAboutToShow: localizeStandardActions()
    onStandardButtonsChanged: Qt.callLater(localizeStandardActions)

    background: Rectangle {
        radius: Theme.controlRadius + 4
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    header: Label {
        visible: text.length > 0
        text: dialog.title
        leftPadding: dialog.leftPadding
        rightPadding: dialog.rightPadding
        topPadding: 20
        bottomPadding: 0
        color: Theme.textPrimary
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }

    footer: DialogButtonBox {
        visible: count > 0
        alignment: Qt.AlignRight
        spacing: 8
        leftPadding: dialog.leftPadding
        rightPadding: dialog.rightPadding
        topPadding: 0
        bottomPadding: 20
        background: Item {}
        delegate: ShadowButton {
            variant: DialogButtonBox.buttonRole === DialogButtonBox.AcceptRole
                || DialogButtonBox.buttonRole === DialogButtonBox.YesRole
                ? ShadowButton.Primary : ShadowButton.Secondary
        }
    }
}
