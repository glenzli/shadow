pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ShadowDialog {
    id: dialog
    required property var profileManager
    required property real hostWidth
    required property real hostHeight
    property bool creating: false
    property url newParent
    property string selectedRoot: ""
    property string selectedId: ""
    property bool selectedAvailable: false

    title: qsTr("Libraries")
    width: Math.min(700, hostWidth - 48)
    height: Math.min(620, hostHeight - 48)
    x: Math.round((hostWidth - width) / 2)
    y: Math.round((hostHeight - height) / 2)
    closePolicy: profileManager.busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    onAboutToShow: {
        profileManager.refresh()
        currentNameField.text = profileManager.currentName
        selectedRoot = profileManager.currentRoot
        selectedId = ""
        selectedAvailable = true
        creating = false
    }

    FolderDialog {
        id: openFolder
        title: qsTr("Open Library")
        onAccepted: dialog.profileManager.openLibrary(selectedFolder)
    }

    FolderDialog {
        id: parentFolder
        title: qsTr("Choose a Location for the New Library")
        onAccepted: dialog.newParent = selectedFolder
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: qsTr("Current Library")
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
        }

        RowLayout {
            Layout.fillWidth: true
            enabled: !dialog.profileManager.busy
            ShadowTextField {
                id: currentNameField
                objectName: "currentLibraryName"
                Layout.fillWidth: true
                maximumLength: 80
                Accessible.name: qsTr("Library Name")
            }
            ShadowButton {
                objectName: "renameLibraryButton"
                text: qsTr("Rename")
                enabled: currentNameField.text.trim().length > 0
                    && currentNameField.text.trim() !== dialog.profileManager.currentName
                onClicked: dialog.profileManager.renameCurrent(currentNameField.text)
            }
        }

        TextEdit {
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            text: dialog.profileManager.currentRoot
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: Text.WrapAnywhere
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            Accessible.name: qsTr("Library Location")
        }

        RowLayout {
            enabled: !dialog.profileManager.busy
            ShadowButton {
                text: qsTr("Show in Folder")
                compact: true
                onClicked: dialog.profileManager.revealCurrent()
            }
            ShadowButton {
                text: qsTr("Copy Location")
                compact: true
                onClicked: dialog.profileManager.copyCurrentLocation()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: dialog.profileManager.debugLaunch
            text: qsTr("Debug remembers its own independent library. Opening the daily library here does not change that startup choice.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        RowLayout {
            Layout.fillWidth: true
            enabled: !dialog.profileManager.busy
            Label {
                Layout.fillWidth: true
                text: dialog.creating ? qsTr("New Library") : qsTr("Recent Libraries")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSubheading
            }
            ShadowButton {
                objectName: "newLibraryButton"
                text: dialog.creating ? qsTr("Back") : qsTr("New Library…")
                compact: true
                onClicked: dialog.creating = !dialog.creating
            }
            ShadowButton {
                objectName: "browseLibraryButton"
                text: qsTr("Open Library…")
                compact: true
                onClicked: openFolder.open()
            }
        }

        ListView {
            id: recentList
            objectName: "recentLibraries"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !dialog.creating
            enabled: !dialog.profileManager.busy
            model: dialog.profileManager.recentProfiles
            clip: true
            spacing: 4
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                id: row
                required property var modelData
                width: recentList.width
                height: 62
                objectName: "recentLibraryRow"
                enabled: modelData.available
                highlighted: dialog.selectedRoot === modelData.root
                Accessible.name: modelData.name
                contentItem: Column {
                    spacing: 3
                    Label {
                        width: parent.width
                        text: row.modelData.current
                            ? qsTr("%1 · Current").arg(row.modelData.name)
                            : row.modelData.available ? row.modelData.name
                            : qsTr("%1 · Unavailable").arg(row.modelData.name)
                        textFormat: Text.PlainText
                        color: row.enabled ? Theme.textPrimary : Theme.textDisabled
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    Label {
                        width: parent.width
                        text: row.modelData.root
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontCaption
                        elide: Text.ElideMiddle
                    }
                }
                background: Rectangle {
                    radius: Theme.controlRadius
                    color: row.highlighted ? Theme.accentSurface
                        : row.hovered ? Theme.buttonHoverSurface : "transparent"
                }
                onClicked: {
                    dialog.selectedRoot = modelData.root
                    dialog.selectedId = modelData.id
                    dialog.selectedAvailable = modelData.available
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.creating
            enabled: !dialog.profileManager.busy
            spacing: 10
            ShadowTextField {
                id: newNameField
                objectName: "newLibraryName"
                Layout.fillWidth: true
                maximumLength: 80
                placeholderText: qsTr("Library Name")
                Accessible.name: placeholderText
            }
            RowLayout {
                Layout.fillWidth: true
                ShadowButton {
                    objectName: "chooseLibraryLocation"
                    text: qsTr("Choose Location…")
                    onClicked: parentFolder.open()
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.newParent.toString().length > 0
                        ? decodeURIComponent(dialog.newParent.toString().replace(/^file:\/\//, ""))
                        : qsTr("No location selected")
                    color: Theme.textMuted
                    elide: Text.ElideMiddle
                    font.pixelSize: Theme.fontCaption
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("A new empty library will be created in this folder. Photos and edits are kept separately in each library.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontCaption
            }
            Item { Layout.fillHeight: true }
        }

        Label {
            objectName: "libraryErrorText"
            Layout.fillWidth: true
            visible: text.length > 0
            text: dialog.profileManager.errorText
            color: Theme.errorText
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontCaption
        }

        Label {
            Layout.fillWidth: true
            text: dialog.profileManager.busy ? dialog.profileManager.statusText
                : qsTr("Switching saves current edits, pauses resumable work and restarts Shadow in the selected library.")
            textFormat: Text.PlainText
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
        }
    }

    footer: Item {
        implicitHeight: footerActions.implicitHeight + 20
        RowLayout {
            id: footerActions
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 8
            layoutDirection: Qt.LeftToRight
            Item { Layout.fillWidth: true }
            ShadowButton {
                objectName: "cancelLibrarySwitchButton"
                visible: dialog.profileManager.canCancel
                text: qsTr("Cancel Switch")
                onClicked: dialog.profileManager.cancelSwitch()
            }
            ShadowButton {
                objectName: "closeLibrariesButton"
                text: qsTr("Done")
                enabled: !dialog.profileManager.busy
                onClicked: dialog.close()
            }
            ShadowButton {
                objectName: "openSelectedLibraryButton"
                text: dialog.creating ? qsTr("Create and Open") : qsTr("Open Selected")
                variant: ShadowButton.Primary
                enabled: !dialog.profileManager.busy
                    && (dialog.creating ? newNameField.text.trim().length > 0
                        && dialog.newParent.toString().length > 0
                        : dialog.selectedAvailable && dialog.selectedRoot !== dialog.profileManager.currentRoot)
                onClicked: {
                    if (dialog.creating)
                        dialog.profileManager.createLibrary(newNameField.text, dialog.newParent)
                    else
                        dialog.profileManager.switchLibrary(dialog.selectedRoot, dialog.selectedId)
                }
            }
        }
    }
}
