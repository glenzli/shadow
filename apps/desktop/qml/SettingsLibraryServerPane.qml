pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ScrollView {
    id: root

    required property var controller
    property string autoStartLabel: qsTr("Start when Shadow opens")

    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    function formatBytes(bytes) {
        const value = Number(bytes || 0)
        if (value < 1024)
            return qsTr("%L1 B").arg(value)
        if (value < 1024 * 1024)
            return qsTr("%1 KB").arg((value / 1024).toLocaleString(Qt.locale(), "f", 1))
        if (value < 1024 * 1024 * 1024)
            return qsTr("%1 MB").arg((value / (1024 * 1024)).toLocaleString(Qt.locale(), "f", 1))
        return qsTr("%1 GB").arg((value / (1024 * 1024 * 1024)).toLocaleString(Qt.locale(), "f", 1))
    }

    function providerLabel() {
        switch (String(controller.providerMode)) {
        case "private":
            return qsTr("Private RAW Provider available")
        case "public-only":
            return qsTr("Public RAW decoding only")
        case "absent":
            return qsTr("Provider Host not installed")
        default:
            return qsTr("Checking Provider Host…")
        }
    }

    function statusMessage() {
        switch (String(controller.statusCode)) {
        case "refreshing": return qsTr("Checking the local server runtime…")
        case "starting": return qsTr("Scanning shared folders and preparing previews…")
        case "stopping": return qsTr("Stopping new connections…")
        case "rescanning": return qsTr("Restarting the server and rescanning shared folders…")
        case "clearing-cache": return qsTr("Clearing rebuildable server previews…")
        case "running": return qsTr("This Mac is available to trusted Shadow clients.")
        case "ready": return qsTr("The server is ready to start.")
        case "stopped": return qsTr("Library sharing has stopped.")
        case "cache-cleared": return qsTr("The server cache was cleared. It will be rebuilt on the next start.")
        case "folder-required": return qsTr("Add at least one shared folder first.")
        case "folder-invalid": return qsTr("Choose an available local folder that is not already shared.")
        case "folder-added": return qsTr("Shared folder added.")
        case "folder-removed": return qsTr("Shared folder removed from future manifests.")
        case "secure-storage-unavailable": return qsTr("Shadow's local credential file is unavailable.")
        case "secret-store-failed": return qsTr("The access token could not be read from Shadow's local file.")
        case "token-copied": return qsTr("Access token copied. Share it only with a trusted device.")
        case "token-regenerated": return qsTr("A new access token was created. Existing clients must reconnect.")
        case "clipboard-unavailable": return qsTr("The access token could not be copied.")
        case "operation-failed": return qsTr("The Library server could not complete the request.")
        default: return ""
        }
    }

    onVisibleChanged: {
        if (visible && !controller.busy)
            controller.refresh()
    }

    FolderDialog {
        id: sharedFolderDialog
        title: qsTr("Choose a folder to share")
        onAccepted: root.controller.addSharedFolder(selectedFolder)
    }

    Dialog {
        id: clearCacheDialog
        anchors.centerIn: parent
        modal: true
        title: qsTr("Clear server cache?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: root.controller.clearCache()

        Label {
            width: 360
            text: qsTr("Only the server Catalog and generated previews are removed. Original photos, shared-folder settings, and server identity remain unchanged.")
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
        }
    }

    component SettingsCard: Rectangle {
        Layout.fillWidth: true
        implicitHeight: cardContent.implicitHeight + 28
        radius: Theme.controlRadius + 2
        color: Theme.panel
        border.width: 1
        border.color: Theme.border
        default property alias content: cardContent.data

        ColumnLayout {
            id: cardContent
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 14

        Label {
            Layout.fillWidth: true
            text: qsTr("Library Server")
            color: Theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Share proxy thumbnails for browsing and transfer an original RAW only when a trusted client opens it for editing.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        SettingsCard {
            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Rectangle {
                    Layout.preferredWidth: 10
                    Layout.preferredHeight: 10
                    radius: 5
                    color: root.controller.running ? Theme.successText : Theme.textMuted
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: root.controller.running ? qsTr("Sharing") : qsTr("Not sharing")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.controller.running && root.controller.localAddress.length > 0
                            ? qsTr("Address · %1").arg(root.controller.localAddress)
                            : qsTr("Start the server after choosing shared folders.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                    }
                }

                BusyIndicator {
                    visible: root.controller.busy
                    running: visible
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                }

                ShadowButton {
                    objectName: "libraryServerPrimaryAction"
                    compact: true
                    variant: root.controller.running ? ShadowButton.Secondary : ShadowButton.Primary
                    text: root.controller.running ? qsTr("Stop Sharing") : qsTr("Start Sharing")
                    enabled: !root.controller.busy
                        && (root.controller.running || root.controller.sharedFolders.length > 0)
                    onClicked: root.controller.running
                        ? root.controller.stopServer() : root.controller.startServer()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.statusMessage()
                color: root.controller.statusCode === "operation-failed"
                    || root.controller.statusCode === "secret-store-failed"
                    ? Theme.dangerText : Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: root.controller.statusCode === "operation-failed"
                    && root.controller.diagnosticText.length > 0
                text: root.controller.diagnosticText
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WrapAnywhere
            }
        }

        SettingsCard {
            Label {
                text: qsTr("Server identity")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                TextField {
                    id: serverNameField
                    objectName: "libraryServerNameField"
                    Layout.fillWidth: true
                    enabled: !root.controller.running && !root.controller.busy
                    text: root.controller.displayName
                    placeholderText: qsTr("Studio Mac")
                    Accessible.name: qsTr("Library server name")
                    onEditingFinished: root.controller.displayName = text
                }

                TextField {
                    id: serverPortField
                    objectName: "libraryServerPortField"
                    Layout.preferredWidth: 110
                    enabled: !root.controller.running && !root.controller.busy
                    text: String(root.controller.port)
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 1024; top: 65535 }
                    Accessible.name: qsTr("Library server port")
                    onEditingFinished: root.controller.port = Number(text)
                }
            }

            RowLayout {
                Layout.fillWidth: true

                ShadowSwitch {
                    compact: true
                    text: qsTr("Allow original RAW downloads")
                    checked: root.controller.servesOriginals
                    enabled: !root.controller.running && !root.controller.busy
                    onToggled: root.controller.servesOriginals = checked
                }

                Item { Layout.fillWidth: true }

                ShadowSwitch {
                    compact: true
                    text: root.autoStartLabel
                    checked: root.controller.autoStart
                    onToggled: root.controller.autoStart = checked
                }
            }
        }

        SettingsCard {
            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Shared folders")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                }

                ShadowButton {
                    objectName: "libraryServerAddFolderButton"
                    compact: true
                    text: qsTr("Add Folder")
                    enabled: !root.controller.running && !root.controller.busy
                    onClicked: sharedFolderDialog.open()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.controller.sharedFolders.length === 0
                text: qsTr("No folders are shared. The server never exposes arbitrary paths from the main Library.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: root.controller.sharedFolders

                delegate: Rectangle {
                    id: folderRow
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 46
                    radius: Theme.controlRadius
                    color: Theme.panelRaised
                    border.width: 1
                    border.color: Theme.border

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 8
                        spacing: 9

                        Rectangle {
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            radius: 4
                            color: folderRow.modelData.available
                                ? Theme.successText : Theme.dangerText
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1

                            Label {
                                Layout.fillWidth: true
                                text: folderRow.modelData.name
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontMeta
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }

                            Label {
                                Layout.fillWidth: true
                                text: folderRow.modelData.available
                                    ? folderRow.modelData.path
                                    : qsTr("Folder unavailable · %1").arg(folderRow.modelData.path)
                                color: folderRow.modelData.available
                                    ? Theme.textMuted : Theme.dangerText
                                font.pixelSize: Theme.fontMeta
                                elide: Text.ElideMiddle
                            }
                        }

                        ShadowButton {
                            compact: true
                            variant: ShadowButton.Ghost
                            text: qsTr("Remove")
                            enabled: !root.controller.running && !root.controller.busy
                            onClicked: root.controller.removeSharedFolder(folderRow.index)
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%L1 indexed photos").arg(root.controller.photoCount)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Rescan")
                    enabled: !root.controller.busy
                        && root.controller.sharedFolders.length > 0
                    onClicked: root.controller.rescanAndRestart()
                }
            }
        }

        SettingsCard {
            Label {
                text: qsTr("Provider, access & cache")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 16
                rowSpacing: 8

                Label { text: qsTr("RAW preview runtime"); color: Theme.textMuted; font.pixelSize: Theme.fontMeta }
                Label { Layout.fillWidth: true; text: root.providerLabel(); color: Theme.textSecondary; font.pixelSize: Theme.fontMeta }
                Label { text: qsTr("Server preview cache"); color: Theme.textMuted; font.pixelSize: Theme.fontMeta }
                Label { Layout.fillWidth: true; text: root.formatBytes(root.controller.cacheByteLength); color: Theme.textSecondary; font.pixelSize: Theme.fontMeta }
                Label { text: qsTr("Access token"); color: Theme.textMuted; font.pixelSize: Theme.fontMeta }
                Label {
                    Layout.fillWidth: true
                    text: root.controller.accessTokenStored ? qsTr("Stored locally") : qsTr("Created on first start")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontMeta
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ShadowButton {
                    objectName: "libraryServerCopyTokenButton"
                    compact: true
                    text: qsTr("Copy Access Token")
                    enabled: root.controller.secureStorageAvailable && !root.controller.busy
                    onClicked: root.controller.copyAccessToken()
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Create New Token")
                    enabled: root.controller.secureStorageAvailable
                        && !root.controller.running && !root.controller.busy
                    onClicked: root.controller.regenerateAccessToken()
                }

                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Danger
                    text: qsTr("Clear Cache")
                    enabled: !root.controller.running && !root.controller.busy
                        && root.controller.cacheByteLength > 0
                    onClicked: clearCacheDialog.open()
                }
            }
        }

        Item { Layout.preferredHeight: 4 }
    }
}
