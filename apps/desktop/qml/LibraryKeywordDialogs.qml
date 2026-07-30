pragma ComponentBehavior: Bound
pragma Translator: "LibraryKeywords"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var controller
    required property real hostWidth

    property string keywordId: ""
    property string keywordName: ""
    property string parentId: ""
    property int keywordDepth: 0
    property int keywordPhotoCount: 0
    property string editMode: "create-root"

    function openCreateRoot() {
        editMode = "create-root"
        keywordId = ""
        keywordName = ""
        parentId = ""
        editPopup.open()
    }

    function openCreateChild(id, name) {
        editMode = "create-child"
        keywordId = ""
        keywordName = ""
        parentId = String(id)
        editPopup.open()
    }

    function openRename(id, name) {
        editMode = "rename"
        keywordId = String(id)
        keywordName = String(name)
        parentId = ""
        editPopup.open()
    }

    function openMove(id, name, currentParentId, depth) {
        keywordId = String(id)
        keywordName = String(name)
        parentId = String(currentParentId)
        keywordDepth = Number(depth)
        movePopup.open()
    }

    function openDelete(id, name, photoCount) {
        keywordId = String(id)
        keywordName = String(name)
        keywordPhotoCount = Number(photoCount)
        deletePopup.open()
    }

    function moveParentOptions() {
        const values = [{
            "id": "",
            "name": qsTr("Top level")
        }]
        const keywords = controller.libraryKeywords
        let skippingDescendants = false
        for (let index = 0; index < keywords.length; ++index) {
            const keyword = keywords[index]
            if (String(keyword.id) === keywordId) {
                skippingDescendants = true
                continue
            }
            if (skippingDescendants
                    && Number(keyword.depth) > keywordDepth) {
                continue
            }
            skippingDescendants = false
            values.push({
                "id": String(keyword.id),
                "name": "\u00a0\u00a0".repeat(Number(keyword.depth))
                    + String(keyword.name)
            })
        }
        return values
    }

    function optionIndex(options, id) {
        for (let index = 0; index < options.length; ++index) {
            if (String(options[index].id) === String(id))
                return index
        }
        return 0
    }

    Popup {
        id: editPopup

        parent: Overlay.overlay
        modal: true
        focus: true
        width: Math.min(360, Math.max(0, root.hostWidth - 40))
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        padding: 16

        onOpened: {
            keywordNameInput.text = root.keywordName
            keywordNameInput.selectAll()
            keywordNameInput.forceActiveFocus()
        }

        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: root.editMode === "rename"
                    ? qsTr("Rename keyword")
                    : root.editMode === "create-child"
                        ? qsTr("Create child keyword")
                        : qsTr("Create keyword")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
            }

            TextField {
                id: keywordNameInput
                Layout.fillWidth: true
                placeholderText: qsTr("Keyword name")
                selectByMouse: true
                onAccepted: confirmEdit.clicked()
            }

            RowLayout {
                Layout.fillWidth: true

                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: editPopup.close()
                }

                ShadowButton {
                    id: confirmEdit
                    compact: true
                    variant: ShadowButton.Primary
                    text: root.editMode === "rename"
                        ? qsTr("Rename") : qsTr("Create")
                    enabled: keywordNameInput.text.trim().length > 0
                        && !root.controller.libraryKeywordsBusy
                    onClicked: {
                        if (root.editMode === "rename") {
                            root.controller.renameLibraryKeyword(
                                root.keywordId, keywordNameInput.text)
                        } else {
                            root.controller.createLibraryKeyword(
                                root.parentId, keywordNameInput.text)
                        }
                        editPopup.close()
                    }
                }
            }
        }
    }

    Popup {
        id: movePopup

        parent: Overlay.overlay
        modal: true
        focus: true
        width: Math.min(380, Math.max(0, root.hostWidth - 40))
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        padding: 16

        onOpened: {
            const options = root.moveParentOptions()
            parentPicker.model = options
            parentPicker.currentIndex = root.optionIndex(
                options, root.parentId)
        }

        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: qsTr("Move “%1”").arg(root.keywordName)
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
            }

            Label {
                text: qsTr("New parent")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }

            ComboBox {
                id: parentPicker
                Layout.fillWidth: true
                textRole: "name"
                valueRole: "id"
            }

            RowLayout {
                Layout.fillWidth: true

                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: movePopup.close()
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Primary
                    text: qsTr("Move")
                    enabled: !root.controller.libraryKeywordsBusy
                    onClicked: {
                        root.controller.moveLibraryKeyword(
                            root.keywordId, parentPicker.currentValue)
                        movePopup.close()
                    }
                }
            }
        }
    }

    Popup {
        id: deletePopup

        parent: Overlay.overlay
        modal: true
        focus: true
        width: Math.min(380, Math.max(0, root.hostWidth - 40))
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        padding: 16

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
                text: qsTr("Delete keyword subtree?")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Delete “%1”, its child keywords, and their assignments from %L2 photos? The photos and edits remain in the Library.")
                    .arg(root.keywordName).arg(root.keywordPhotoCount)
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true

                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: deletePopup.close()
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Danger
                    text: qsTr("Delete keyword")
                    enabled: !root.controller.libraryKeywordsBusy
                    onClicked: {
                        root.controller.deleteLibraryKeyword(root.keywordId)
                        deletePopup.close()
                    }
                }
            }
        }
    }
}
