pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Popup {
    id: root

    required property var preferences
    required property real hostWidth
    required property real hostHeight
    readonly property bool libraryMapReady: settingsPane.libraryMapReady

    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(580, Math.max(0, root.hostWidth - 48))
    height: Math.min(670, Math.max(0, root.hostHeight - 48))
    x: Math.round(((parent ? parent.width : root.hostWidth) - width) / 2)
    y: Math.round(((parent ? parent.height : root.hostHeight) - height) / 2)
    padding: 22
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: settingsPane.discardSecretDraft()

    function present() {
        open()
        settingsPane.prepare()
    }

    background: Rectangle {
        radius: Theme.controlRadius + 2
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: MapProviderSettingsPane {
        id: settingsPane
        preferences: root.preferences
        showDoneButton: true
        onDoneRequested: root.close()
    }
}
