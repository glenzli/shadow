pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var editor

    implicitWidth: 152
    implicitHeight: 30

    function displayName(variant) {
        return variant.name.trim().length > 0 ? variant.name : qsTr("Original")
    }

    function menuName(variant) {
        const name = root.displayName(variant)
        return variant.isDefault && variant.name.trim().length > 0
            ? name + qsTr(" · Original") : name
    }

    function activeName() {
        const variants = root.editor.photoVariants
        for (let index = 0; index < variants.length; ++index) {
            if (variants[index].isActive)
                return root.displayName(variants[index])
        }
        return qsTr("Original")
    }

    ShadowButton {
        objectName: "precisionVariantSelectorButton"
        anchors.fill: parent
        compact: true
        variant: ShadowButton.Secondary
        text: root.activeName()
        toolTipText: qsTr("Photo variants")
        enabled: root.editor.active
        onClicked: variantMenu.open()
    }

    Popup {
        id: variantMenu
        objectName: "precisionVariantMenu"
        parent: root
        x: root.width - width
        y: root.height + 6
        width: 270
        padding: 8
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 4

            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                Layout.bottomMargin: 4
                text: qsTr("Photo variants")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.Medium
            }

            Repeater {
                model: root.editor.photoVariants

                delegate: RowLayout {
                    id: variantRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 2

                    ShadowButton {
                        Layout.fillWidth: true
                        compact: true
                        variant: ShadowButton.Ghost
                        selected: variantRow.modelData.isActive
                        text: root.menuName(variantRow.modelData)
                        enabled: root.editor.variantActionsEnabled
                            || variantRow.modelData.isActive
                        onClicked: {
                            variantMenu.close()
                            root.editor.activateVariant(
                                variantRow.modelData.variantId)
                        }
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/edit.svg"
                        variant: ShadowIconButton.Ghost
                        buttonSize: 28
                        iconSize: 14
                        toolTipText: qsTr("Rename variant")
                        enabled: root.editor.variantActionsEnabled
                        onClicked: namePopup.beginRename(
                            variantRow.modelData.variantId,
                            root.displayName(variantRow.modelData))
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/trash.svg"
                        variant: ShadowIconButton.Ghost
                        buttonSize: 28
                        iconSize: 14
                        toolTipText: variantRow.modelData.isDefault
                            ? qsTr("The original variant is retained")
                            : variantRow.modelData.isActive
                                ? qsTr("Switch variants before removing this one")
                                : qsTr("Remove variant")
                        enabled: root.editor.variantActionsEnabled
                            && !variantRow.modelData.isDefault
                            && !variantRow.modelData.isActive
                        onClicked: root.editor.removeVariant(
                            variantRow.modelData.variantId)
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                height: 1
                color: Theme.border
            }

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                variant: ShadowButton.Ghost
                text: qsTr("New variant…")
                enabled: root.editor.variantActionsEnabled
                onClicked: namePopup.beginCreate()
            }
        }
    }

    Popup {
        id: namePopup

        property string variantId: ""
        property bool renaming: false

        function beginCreate() {
            variantMenu.close()
            renaming = false
            variantId = ""
            nameField.text = qsTr("Variant %1").arg(
                root.editor.photoVariants.length + 1)
            open()
            nameField.forceActiveFocus()
            nameField.selectAll()
        }

        function beginRename(variantId, currentName) {
            variantMenu.close()
            renaming = true
            namePopup.variantId = variantId
            nameField.text = currentName
            open()
            nameField.forceActiveFocus()
            nameField.selectAll()
        }

        function acceptName() {
            const name = nameField.text.trim()
            if (name.length === 0)
                return
            if (renaming)
                root.editor.renameVariant(variantId, name)
            else
                root.editor.createVariant(name)
            close()
        }

        parent: root
        x: root.width - width
        y: root.height + 6
        width: 270
        padding: 12
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                text: namePopup.renaming
                    ? qsTr("Rename variant") : qsTr("New variant")
                color: Theme.textPrimary
                font.weight: Font.Medium
            }

            ShadowTextField {
                id: nameField
                Layout.fillWidth: true
                color: Theme.textPrimary
                selectionColor: Theme.accentSurface
                selectedTextColor: Theme.textPrimary
                placeholderText: qsTr("Variant name")
                selectByMouse: true
                onAccepted: namePopup.acceptName()

                background: Rectangle {
                    radius: Theme.compactControlRadius
                    color: Theme.control
                    border.width: 1
                    border.color: nameField.activeFocus
                        ? Theme.focusRing : Theme.borderStrong
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 6

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Cancel")
                    onClicked: namePopup.close()
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Primary
                    text: namePopup.renaming ? qsTr("Rename") : qsTr("Create")
                    enabled: nameField.text.trim().length > 0
                        && root.editor.variantActionsEnabled
                    onClicked: namePopup.acceptName()
                }
            }
        }
    }
}
