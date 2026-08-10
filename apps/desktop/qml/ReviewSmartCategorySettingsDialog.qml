pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: dialog
    required property var controller
    property int selectedIndex: 0
    property var selectedCategory: controller.categories.length > selectedIndex
        && selectedIndex >= 0 ? controller.categories[selectedIndex] : null

    function loadSelectedCategory() {
        if (selectedCategory === null) {
            nameField.text = ""
            descriptionField.text = ""
            thresholdSlider.value = 0.05
            enabledCheck.checked = true
            return
        }
        nameField.text = String(selectedCategory.name)
        descriptionField.text = String(selectedCategory.description)
        thresholdSlider.value = Number(selectedCategory.minimumSimilarity)
        enabledCheck.checked = Boolean(selectedCategory.enabled)
    }
    onSelectedCategoryChanged: Qt.callLater(loadSelectedCategory)
    onOpened: loadSelectedCategory()

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(620, parent ? parent.width - 48 : 620)
    height: Math.min(520, parent ? parent.height - 48 : 520)
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.panelRaised
        border.color: Theme.borderStrong
        border.width: 1
        radius: Theme.controlRadius + 2
    }

    contentItem: ColumnLayout {
        clip: true
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 18
            Label {
                Layout.fillWidth: true
                text: qsTr("Smart categories")
                color: Theme.textPrimary
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            ShadowIconButton {
                source: "qrc:/icons/node-add.svg"
                buttonSize: 28
                iconSize: 14
                toolTipText: qsTr("Add category")
                accessibleName: toolTipText
                onClicked: dialog.selectedIndex = -1
            }
            ShadowIconButton {
                source: "qrc:/icons/close.svg"
                buttonSize: 28
                iconSize: 14
                toolTipText: qsTr("Close")
                accessibleName: toolTipText
                onClicked: dialog.close()
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            spacing: 0
            ListView {
                id: settingsList
                Layout.preferredWidth: 205
                Layout.fillHeight: true
                Layout.margins: 12
                spacing: 2
                clip: true
                model: dialog.controller.categories
                delegate: ItemDelegate {
                    id: categoryButton
                    required property var modelData
                    required property int index
                    width: settingsList.width
                    height: 38
                    leftPadding: 10
                    rightPadding: 10
                    topPadding: 0
                    bottomPadding: 0
                    hoverEnabled: true
                    Accessible.name: String(modelData.name)
                    onClicked: dialog.selectedIndex = index

                    background: Rectangle {
                        radius: Theme.compactControlRadius
                        color: categoryButton.index === dialog.selectedIndex
                            ? Theme.accentSurface
                            : categoryButton.hovered
                                ? Theme.buttonGhostHover : Theme.transparent
                        border.width: categoryButton.visualFocus ? 1 : 0
                        border.color: Theme.focusRing
                    }

                    contentItem: Label {
                        text: String(categoryButton.modelData.name)
                        color: categoryButton.index === dialog.selectedIndex
                            ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                        font.weight: categoryButton.index === dialog.selectedIndex
                            ? Font.DemiBold : Font.Medium
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                }
            }
            Rectangle {
                Layout.fillHeight: true
                Layout.minimumHeight: 0
                Layout.preferredWidth: 1
                Layout.bottomMargin: 1
                color: Theme.border
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 20
                spacing: 10
                Label { text: qsTr("Name"); color: Theme.textMuted; font.pixelSize: 10 }
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    selectByMouse: true
                }
                Label { text: qsTr("Model description"); color: Theme.textMuted; font.pixelSize: 10 }
                TextArea {
                    id: descriptionField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 90
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Match threshold"); color: Theme.textMuted; font.pixelSize: 10 }
                    Slider {
                        id: thresholdSlider
                        Layout.fillWidth: true
                        from: -0.05; to: 0.20; stepSize: 0.01
                        value: 0.05
                    }
                    Label { text: thresholdSlider.value.toFixed(2); color: Theme.textPrimary }
                }
                ShadowCheckBox {
                    id: enabledCheck
                    text: qsTr("Show this category")
                    checked: true
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Photos may appear in more than one category. Changes are matched locally using the existing image-vector cache.")
                    color: Theme.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.Wrap
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.preferredHeight: 1
                    color: Theme.border
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            text: qsTr("Rebuild analysis")
                            color: Theme.textPrimary
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Recreate image vectors and all smart-category matches.")
                            color: Theme.textMuted
                            font.pixelSize: 9
                            wrapMode: Text.Wrap
                        }
                    }
                    ShadowButton {
                        text: qsTr("Rebuild")
                        variant: ShadowButton.Ghost
                        enabled: !dialog.controller.busy
                        onClicked: dialog.controller.rebuild()
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: dialog.controller.busy || dialog.controller.failed
                    spacing: 8
                    BusyIndicator {
                        visible: dialog.controller.busy
                        running: visible
                        Layout.preferredWidth: 14
                        Layout.preferredHeight: 14
                    }
                    Label {
                        Layout.fillWidth: true
                        text: dialog.controller.failed
                            ? dialog.controller.errorText : dialog.controller.statusText
                        color: dialog.controller.failed ? Theme.errorText : Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.Wrap
                    }
                }
                Item { Layout.fillHeight: true }
                RowLayout {
                    Layout.fillWidth: true
                    ShadowButton {
                        text: qsTr("Reset defaults")
                        variant: ShadowButton.Ghost
                        onClicked: dialog.controller.resetDefaults()
                    }
                    Item { Layout.fillWidth: true }
                    ShadowButton {
                        text: qsTr("Save")
                        variant: ShadowButton.Primary
                        enabled: nameField.text.trim().length > 0
                            && descriptionField.text.trim().length > 0
                        onClicked: {
                            if (dialog.selectedCategory === null) {
                                dialog.controller.addCategory(nameField.text,
                                    descriptionField.text)
                                dialog.selectedIndex =
                                    dialog.controller.categories.length - 1
                            } else {
                                const id = String(dialog.selectedCategory.id)
                                dialog.controller.updateCategory(id, nameField.text,
                                    descriptionField.text, thresholdSlider.value,
                                    enabledCheck.checked)
                            }
                            dialog.close()
                        }
                    }
                }
            }
        }
    }
}
