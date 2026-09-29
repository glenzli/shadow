pragma ComponentBehavior: Bound
pragma Translator: "Main"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: recovery

    required property var editor
    required property var hostWindow

    property bool closeAfterAutosave: false
    property bool openingPendingPhoto: false

    function shouldAcceptClose() {
        return closeAfterAutosave || editor.prepareToClose()
    }

    function keepEditing() {
        if (openingPendingPhoto)
            editor.cancelPendingPhotoOpen()
        close()
    }

    function retrySave() {
        const pendingPhoto = openingPendingPhoto
        close()
        editor.retryAutosave()
        if (!pendingPhoto)
            Qt.callLater(hostWindow.close)
    }

    function continueWithoutSaving() {
        const pendingPhoto = openingPendingPhoto
        close()
        if (pendingPhoto) {
            editor.discardFailedAutosaveAndOpenPendingPhoto()
        } else {
            closeAfterAutosave = true
            Qt.callLater(hostWindow.close)
        }
    }

    Connections {
        target: recovery.hostWindow
        ignoreUnknownSignals: true

        function onClosing(closeEvent) {
            if (!recovery.shouldAcceptClose())
                closeEvent.accepted = false
        }
    }

    Connections {
        target: recovery.editor

        function onCloseReady() {
            recovery.closeAfterAutosave = true
            Qt.callLater(recovery.hostWindow.close)
        }

        function onCloseSaveFailed() {
            // Keep the draft alive and make the consequence of bypassing the
            // durable save explicit. Never enter an automatic retry loop.
            recovery.closeAfterAutosave = false
            recovery.openingPendingPhoto = false
            recovery.open()
        }

        function onPhotoSwitchSaveFailed() {
            // The requested photo remains queued while the current in-memory
            // draft waits for one explicit recovery decision.
            recovery.openingPendingPhoto = true
            recovery.open()
        }
    }

    x: Math.round(((parent ? parent.width : width) - width) / 2)
    y: Math.round(((parent ? parent.height : height) - height) / 2)
    width: Math.min(470, (parent ? parent.width : 518) - 48)
    padding: 0
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.NoAutoClose

    background: Rectangle {
        radius: Theme.controlRadius + 2
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.errorBorder
    }

    contentItem: ColumnLayout {
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 22
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("AUTOSAVE FAILED")
                color: Theme.errorText
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
                font.letterSpacing: 1.1
            }

            Label {
                Layout.fillWidth: true
                text: recovery.openingPendingPhoto
                    ? qsTr("Shadow could not save this photo’s latest working adjustments. The selected photo will remain unopened until you retry, keep editing, or open it without these unsaved changes.")
                    : qsTr("Shadow could not save the latest working adjustments locally. You can retry, keep editing, or quit without the unsaved changes.")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
                lineHeight: 1.35
            }

            Label {
                Layout.fillWidth: true
                visible: recovery.editor.autosaveErrorText.length > 0
                text: recovery.editor.autosaveErrorText
                color: Theme.textMuted
                font.pixelSize: Theme.fontSection
                wrapMode: Text.WordWrap
                lineHeight: 1.3
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            spacing: 8

            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("KEEP EDITING")
                variant: ShadowButton.Secondary
                onClicked: recovery.keepEditing()
            }

            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("RETRY SAVE")
                variant: ShadowButton.Primary
                enabled: !recovery.editor.stateBusy
                onClicked: recovery.retrySave()
            }

            ShadowButton {
                Layout.fillWidth: true
                text: recovery.openingPendingPhoto
                    ? qsTr("OPEN WITHOUT SAVING")
                    : qsTr("QUIT WITHOUT SAVING")
                variant: ShadowButton.Danger
                enabled: !recovery.editor.stateBusy
                onClicked: recovery.continueWithoutSaving()
            }
        }
    }
}
