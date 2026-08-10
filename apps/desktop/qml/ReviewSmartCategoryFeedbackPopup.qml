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
    property var categoryOptions: []
    readonly property bool hasWorkspace:
        workspace !== null && workspace !== undefined

    width: 354
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
        categoryOptions = workspace.smartCategoryController.feedbackCategories(
            photoId, representationId)
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
        categoryOptions = []
    }

    function decide(categoryId, decision) {
        if (!hasWorkspace)
            return
        workspace.smartCategoryController.recordFeedback(
            photoId, representationId, String(categoryId), decision)
        close()
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
                text: qsTr("Review smart category")
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
                text: qsTr("Your decision is kept as the strongest local evidence.")
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
            Layout.preferredHeight: Math.min(contentHeight, 300)
            clip: true
            spacing: 2
            model: root.categoryOptions

            delegate: Rectangle {
                id: categoryRow
                required property var modelData
                width: categoryList.width
                height: 42
                radius: Theme.compactControlRadius
                color: Boolean(modelData.uncertain)
                    ? Theme.warningSurface : Theme.transparent
                border.width: Boolean(modelData.uncertain) ? 1 : 0
                border.color: Theme.warningBorder

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 9
                    anchors.rightMargin: 6
                    spacing: 6

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: String(categoryRow.modelData.name)
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSection
                            elide: Text.ElideRight
                        }
                        Label {
                            visible: Boolean(categoryRow.modelData.uncertain)
                            text: qsTr("HIGH-VALUE REVIEW")
                            color: Theme.warningText
                            font.pixelSize: 8
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.7
                        }
                    }

                    ShadowButton {
                        text: qsTr("No")
                        variant: ShadowButton.Ghost
                        onClicked: root.decide(categoryRow.modelData.id, -1)
                    }
                    ShadowButton {
                        text: qsTr("Yes")
                        variant: ShadowButton.Secondary
                        onClicked: root.decide(categoryRow.modelData.id, 1)
                    }
                    ShadowIconButton {
                        source: "qrc:/icons/undo.svg"
                        buttonSize: 28
                        iconSize: 13
                        toolTipText: qsTr("Clear previous decision")
                        accessibleName: toolTipText
                        onClicked: root.decide(categoryRow.modelData.id, 0)
                    }
                }
            }
        }
    }
}
