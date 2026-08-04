pragma ComponentBehavior: Bound
pragma Translator: LibraryWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Presents local and remote Libraries as peer sources. Connection identity,
// credentials, synchronization, and proxy-cache policy remain in the
// ReviewRemoteLibraryCoordinator and its connection store.
ColumnLayout {
    id: root

    required property var controller
    property string pendingRemovalId: ""
    property string pendingRemovalName: ""
    property string pendingEditId: ""
    property bool pendingEditTokenStored: false
    readonly property var remoteLibraries: controller.remoteLibraries

    spacing: 10

    function statusText(connection) {
        switch (String(connection.statusCode)) {
        case "offline-ready":
            return connection.photoCount > 0 ? qsTr("Offline cache ready · %L1 photos").arg(connection.photoCount) : qsTr("Connected · no cached photos yet");
        case "synchronizing":
            return qsTr("Synchronizing…");
        case "synchronized":
            return qsTr("Up to date · %L1 photos").arg(connection.photoCount);
        case "connection-saved":
            return qsTr("Saved securely · synchronization queued");
        case "token-required":
            return qsTr("Access token unavailable · reconnect this Library");
        case "sync-failed":
            return qsTr("Offline · cached thumbnails remain available");
        case "cache-load-failed":
            return qsTr("Local proxy cache could not be opened");
        case "remote-original-unavailable":
            return qsTr("Browsing only · original downloads are disabled");
        default:
            return connection.hasCachedServer ? qsTr("Offline cache · %L1 photos").arg(connection.photoCount) : qsTr("Ready to synchronize");
        }
    }

    function statusColor(connection) {
        const code = String(connection.statusCode);
        if (code.indexOf("failed") >= 0 || code === "token-required")
            return Theme.dangerText;
        if (code === "synchronized" || code === "offline-ready")
            return Theme.successText;
        return Theme.textMuted;
    }

    function admissionStatusText() {
        switch (String(controller.remoteLibraryStatusCode)) {
        case "invalid-address":
            return qsTr("Enter a server address such as 192.168.1.20:45321.");
        case "invalid-token":
            return qsTr("Enter the access token configured by Shadow Server.");
        case "duplicate-address":
            return qsTr("This server address is already in the Library list.");
        case "secure-storage-unavailable":
            return qsTr("Secure credential storage is unavailable on this Mac.");
        case "secret-store-failed":
            return qsTr("The credential store could not complete the request.");
        case "connection-save-failed":
            return qsTr("The remote Library connection could not be saved.");
        case "operation-busy":
            return qsTr("Wait for the current remote Library operation to finish.");
        default:
            return "";
        }
    }

    Item {
        Layout.preferredWidth: 0
        Layout.preferredHeight: 0

        Popup {
            id: removePopup
            parent: Overlay.overlay
            x: parent ? Math.round((parent.width - width) / 2) : 0
            y: parent ? Math.round((parent.height - height) / 2) : 0
            width: 390
            padding: 18
            modal: true
            focus: true
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

            background: Rectangle {
                radius: 10
                color: Theme.panelRaised
                border.width: 1
                border.color: Theme.borderStrong
            }

            contentItem: ColumnLayout {
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Remove remote Library?")
                    color: Theme.textPrimary
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 will leave this Library view. Its downloaded originals stay in the local Library, and its proxy cache can be reused if you add it again.").arg(root.pendingRemovalName)
                    color: Theme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true

                    Item {
                        Layout.fillWidth: true
                    }

                    ShadowButton {
                        compact: true
                        text: qsTr("CANCEL")
                        onClicked: removePopup.close()
                    }

                    ShadowButton {
                        objectName: "remoteLibraryRemoveConfirmButton"
                        compact: true
                        variant: ShadowButton.Danger
                        text: qsTr("REMOVE LIBRARY")
                        enabled: !root.controller.remoteLibraryBusy
                        onClicked: {
                            root.controller.removeRemoteLibraryConnection(root.pendingRemovalId);
                            removePopup.close();
                        }
                    }
                }
            }
        }

        Popup {
            id: editPopup
            parent: Overlay.overlay
            x: parent ? Math.round((parent.width - width) / 2) : 0
            y: parent ? Math.round((parent.height - height) / 2) : 0
            width: 430
            padding: 18
            modal: true
            focus: true
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            onClosed: editAccessTokenField.clear()

            background: Rectangle {
                radius: 10
                color: Theme.panelRaised
                border.width: 1
                border.color: Theme.borderStrong
            }

            contentItem: ColumnLayout {
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Edit Remote Library")
                    color: Theme.textPrimary
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Update the server address or replace its secure access token. Cached thumbnails keep the same Library identity.")
                    color: Theme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                TextField {
                    id: editServerAddressField
                    objectName: "remoteLibraryEditAddressField"
                    Layout.fillWidth: true
                    enabled: !root.controller.remoteLibraryBusy
                    placeholderText: qsTr("Mac address · 192.168.1.20:45321")
                    selectByMouse: true
                    Accessible.name: qsTr("Remote Library server address")
                }

                TextField {
                    id: editAccessTokenField
                    objectName: "remoteLibraryEditTokenField"
                    Layout.fillWidth: true
                    enabled: root.controller.remoteLibrarySecureStorageAvailable && !root.controller.remoteLibraryBusy
                    echoMode: TextInput.Password
                    passwordCharacter: "•"
                    placeholderText: root.pendingEditTokenStored ? qsTr("Leave blank to keep the saved token") : qsTr("Access token required")
                    selectByMouse: true
                    Accessible.name: qsTr("Replacement remote Library access token")
                }

                Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.admissionStatusText()
                    color: Theme.dangerText
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true

                    Item {
                        Layout.fillWidth: true
                    }

                    ShadowButton {
                        compact: true
                        text: qsTr("CANCEL")
                        onClicked: editPopup.close()
                    }

                    ShadowButton {
                        objectName: "remoteLibraryEditSaveButton"
                        compact: true
                        variant: ShadowButton.Primary
                        text: qsTr("SAVE & SYNC")
                        enabled: root.controller.remoteLibrarySecureStorageAvailable && !root.controller.remoteLibraryBusy && editServerAddressField.text.trim().length > 0 && (root.pendingEditTokenStored || editAccessTokenField.text.trim().length > 0)
                        onClicked: {
                            const connectionId = root.controller.saveRemoteLibraryConnection(root.pendingEditId, editServerAddressField.text, editAccessTokenField.text);
                            if (connectionId.length > 0)
                                editPopup.close();
                        }
                    }
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true

        Label {
            text: qsTr("LIBRARIES")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            font.weight: Font.DemiBold
            font.letterSpacing: 0.5
        }

        Item {
            Layout.fillWidth: true
        }

        ShadowButton {
            objectName: "remoteLibrarySyncAllButton"
            visible: root.remoteLibraries.length > 1
            compact: true
            variant: ShadowButton.Ghost
            text: qsTr("SYNC ALL")
            enabled: !root.controller.remoteLibraryBusy
            onClicked: root.controller.syncAllRemoteLibraries()
        }
    }

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: localContent.implicitHeight + 28
        radius: 8
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.border

        RowLayout {
            id: localContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: 8
                color: Theme.accentSurfaceQuiet

                ShadowIcon {
                    anchors.centerIn: parent
                    source: "qrc:/icons/add-folder.svg"
                    color: Theme.accent
                    size: 17
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Local Library")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSection
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("This Mac · local folders and downloaded originals")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }
            }

            Label {
                text: qsTr("LOCAL")
                color: Theme.accentTextMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
        }
    }

    Repeater {
        model: root.remoteLibraries.length

        delegate: Rectangle {
            id: remoteRow
            required property int index
            readonly property var connection: root.remoteLibraries[index]

            Layout.fillWidth: true
            implicitHeight: remoteContent.implicitHeight + 28
            radius: 8
            color: Theme.panelRaised
            border.width: 1
            border.color: connection.busy ? Theme.accentBorder : Theme.border

            RowLayout {
                id: remoteContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    radius: 8
                    color: Theme.panelInset

                    ShadowIcon {
                        anchors.centerIn: parent
                        source: "qrc:/icons/shared-link.svg"
                        color: remoteRow.connection.hasCachedServer ? Theme.successText : Theme.textMuted
                        size: 17
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: remoteRow.connection.serverName.length > 0 ? remoteRow.connection.serverName : remoteRow.connection.address
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSection
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: remoteRow.connection.serverName.length > 0 ? remoteRow.connection.address : qsTr("Remote Library")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        elide: Text.ElideMiddle
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.statusText(remoteRow.connection)
                        color: root.statusColor(remoteRow.connection)
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                        ToolTip.visible: diagnosticHover.hovered && remoteRow.connection.diagnosticText.length > 0
                        ToolTip.text: remoteRow.connection.diagnosticText

                        HoverHandler {
                            id: diagnosticHover
                        }
                    }
                }

                BusyIndicator {
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    visible: remoteRow.connection.busy
                    running: visible
                }

                ShadowButton {
                    objectName: "remoteLibraryEditButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("EDIT")
                    enabled: !root.controller.remoteLibraryBusy
                    onClicked: {
                        root.pendingEditId = remoteRow.connection.id;
                        root.pendingEditTokenStored = remoteRow.connection.tokenStored;
                        editServerAddressField.text = remoteRow.connection.address;
                        editAccessTokenField.clear();
                        editPopup.open();
                    }
                }

                ShadowButton {
                    objectName: "remoteLibrarySyncButton"
                    compact: true
                    text: qsTr("SYNC")
                    enabled: remoteRow.connection.tokenStored && !root.controller.remoteLibraryBusy
                    onClicked: root.controller.syncRemoteLibrary(remoteRow.connection.id)
                }

                ShadowButton {
                    objectName: "remoteLibraryRemoveButton"
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("REMOVE")
                    enabled: !root.controller.remoteLibraryBusy
                    onClicked: {
                        root.pendingRemovalId = remoteRow.connection.id;
                        root.pendingRemovalName = remoteRow.connection.serverName.length > 0 ? remoteRow.connection.serverName : remoteRow.connection.address;
                        removePopup.open();
                    }
                }
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: addContent.implicitHeight + 28
        radius: 8
        color: Theme.panel
        border.width: 1
        border.color: Theme.border

        ColumnLayout {
            id: addContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("Add Remote Library")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow keeps an independent proxy cache and secure credential for every Library. The original RAW is fetched only when editing begins.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: serverAddressField
                    objectName: "remoteLibraryServerAddressField"
                    Layout.fillWidth: true
                    enabled: !root.controller.remoteLibraryBusy
                    placeholderText: qsTr("Mac address · 192.168.1.20:45321")
                    selectByMouse: true
                    Accessible.name: qsTr("Remote Library server address")
                }

                TextField {
                    id: accessTokenField
                    objectName: "remoteLibraryAccessTokenField"
                    Layout.fillWidth: true
                    enabled: root.controller.remoteLibrarySecureStorageAvailable && !root.controller.remoteLibraryBusy
                    echoMode: TextInput.Password
                    passwordCharacter: "•"
                    placeholderText: qsTr("Access token")
                    selectByMouse: true
                    Accessible.name: qsTr("Remote Library access token")
                    onAccepted: {
                        if (addButton.enabled)
                            addButton.clicked();
                    }
                }

                ShadowButton {
                    id: addButton
                    objectName: "remoteLibraryConnectionAddButton"
                    compact: true
                    variant: ShadowButton.Primary
                    text: qsTr("ADD & SYNC")
                    enabled: root.controller.remoteLibrarySecureStorageAvailable && !root.controller.remoteLibraryBusy && serverAddressField.text.trim().length > 0 && accessTokenField.text.trim().length > 0
                    onClicked: {
                        const connectionId = root.controller.saveRemoteLibraryConnection("", serverAddressField.text, accessTokenField.text);
                        if (connectionId.length > 0) {
                            serverAddressField.clear();
                            accessTokenField.clear();
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: !root.controller.remoteLibrarySecureStorageAvailable
                text: qsTr("Secure credential storage is unavailable on this Mac.")
                color: Theme.dangerText
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: root.controller.remoteLibrarySecureStorageAvailable && text.length > 0
                text: root.admissionStatusText()
                color: Theme.dangerText
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }
        }
    }
}
