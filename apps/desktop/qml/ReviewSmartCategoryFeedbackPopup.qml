pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property var workspace: null
    property string photoId: ""
    property string representationId: ""
    property string photoTitle: ""
    property int modelRevision: 0
    readonly property bool hasWorkspace:
        workspace !== null && workspace !== undefined
    readonly property bool hasChanges: {
        const revision = modelRevision
        for (let index = 0; index < categoryModel.count; ++index) {
            const option = categoryModel.get(index)
            if (Boolean(option.chosen) !== Boolean(option.originalChosen))
                return revision >= 0
        }
        return false
    }

    width: 388
    padding: 10
    modal: false
    focus: true
    parent: Overlay.overlay
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function openFor(item, workspaceValue, photoIdValue,
                     representationIdValue, titleValue) {
        workspace = workspaceValue
        photoId = String(photoIdValue)
        representationId = String(representationIdValue)
        photoTitle = String(titleValue)
        categoryModel.clear()
        const options = workspace.smartCategoryController.feedbackCategories(
            photoId, representationId)
        for (let index = 0; index < options.length; ++index) {
            const option = options[index]
            const matched = Boolean(option.matched)
            categoryModel.append({
                "categoryId": String(option.id),
                "categoryName": String(option.name),
                "uncertain": Boolean(option.uncertain),
                "originalChosen": matched,
                "chosen": matched
            })
        }
        ++modelRevision
        parent = Overlay.overlay
        const point = item.mapToItem(Overlay.overlay, item.width, 0)
        x = Math.max(8, Math.min(point.x - width,
            workspace.width - width - 8))
        y = Math.max(8, Math.min(point.y,
            workspace.height - implicitHeight - 8))
        open()
    }

    function releaseOwner(ownerItem) {
        close()
        if (ownerItem)
            parent = ownerItem
        workspace = null
        photoId = ""
        representationId = ""
        photoTitle = ""
        categoryModel.clear()
        ++modelRevision
    }

    function saveCorrections() {
        if (!hasWorkspace)
            return
        for (let index = 0; index < categoryModel.count; ++index) {
            const option = categoryModel.get(index)
            if (Boolean(option.chosen) === Boolean(option.originalChosen))
                continue
            workspace.smartCategoryController.recordFeedback(
                photoId, representationId, String(option.categoryId),
                Boolean(option.chosen) ? 1 : -1)
        }
        close()
    }

    ListModel {
        id: categoryModel
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: ColumnLayout {
        width: root.width - root.leftPadding - root.rightPadding
        spacing: 8

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            spacing: 2

            Label {
                Layout.fillWidth: true
                text: qsTr("Correct smart categories")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: root.photoTitle
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideMiddle
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Select every category that correctly describes this photo.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.border
        }

        ListView {
            id: categoryList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 320)
            clip: true
            spacing: 2
            model: categoryModel

            delegate: Rectangle {
                id: categoryRow
                required property int index
                required property string categoryId
                required property string categoryName
                required property bool uncertain
                required property bool originalChosen
                required property bool chosen
                width: categoryList.width
                height: 44
                radius: Theme.compactControlRadius
                color: categoryRow.uncertain
                    ? Theme.warningSurface : Theme.transparent
                border.width: categoryRow.uncertain ? 1 : 0
                border.color: Theme.warningBorder

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 9
                    anchors.rightMargin: 9
                    spacing: 8

                    ShadowCheckBox {
                        Layout.fillWidth: true
                        compact: true
                        text: categoryRow.categoryName
                        checked: categoryRow.chosen
                        accessibleName: qsTr("%1 belongs to this photo")
                            .arg(categoryRow.categoryName)
                        onToggled: {
                            if (checked === categoryRow.chosen)
                                return
                            categoryModel.setProperty(
                                categoryRow.index, "chosen", checked)
                            ++root.modelRevision
                        }
                    }

                    Label {
                        visible: categoryRow.uncertain
                        text: qsTr("NEEDS REVIEW")
                        color: Theme.warningText
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.6
                    }

                    Label {
                        visible: !categoryRow.uncertain
                            && categoryRow.originalChosen
                        text: qsTr("CURRENT")
                        color: Theme.textMuted
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.6
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            text: qsTr("Your corrections are saved locally and help classify similar photos.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            ShadowButton {
                text: qsTr("Cancel")
                variant: ShadowButton.Ghost
                onClicked: root.close()
            }

            ShadowButton {
                text: qsTr("Save corrections")
                variant: ShadowButton.Primary
                enabled: root.hasChanges
                onClicked: root.saveCorrections()
            }
        }
    }
}
