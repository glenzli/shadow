pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: smartCategories
    required property var workspace
    readonly property var controller: workspace.smartCategoryController
    spacing: 4

    ReviewSmartCategorySettingsDialog {
        id: settingsDialog
        controller: smartCategories.controller
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 10
        spacing: 4

        Label {
            Layout.fillWidth: true
            text: qsTr("SMART CATEGORIES")
            color: smartCategories.workspace.textMuted
            font.pixelSize: 9
            font.weight: Font.DemiBold
            font.letterSpacing: 1.1
        }
        BusyIndicator {
            visible: smartCategories.controller.busy
            running: visible
            Layout.preferredWidth: 13
            Layout.preferredHeight: 13
        }
        ShadowIconButton {
            visible: !smartCategories.controller.busy
            source: "qrc:/icons/refresh.svg"
            buttonSize: 22
            iconSize: 12
            toolTipText: qsTr("Update smart categories")
            accessibleName: toolTipText
            onClicked: smartCategories.controller.refresh()
        }
        ShadowIconButton {
            source: "qrc:/icons/settings.svg"
            buttonSize: 22
            iconSize: 13
            toolTipText: qsTr("Manage smart categories")
            accessibleName: toolTipText
            onClicked: settingsDialog.open()
        }
    }

    ProgressBar {
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? 3 : 0
        visible: smartCategories.controller.busy
        from: 0
        to: 100
        value: smartCategories.controller.progressPercent
    }

    Rectangle {
        id: uncertaintyRow
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? 32 : 0
        visible: smartCategories.controller.uncertainCount > 0
        radius: Theme.compactControlRadius
        color: smartCategories.controller.reviewingUncertain
            ? Theme.warningSurface
            : uncertaintyMouse.containsMouse ? Theme.buttonGhostHover
            : Theme.transparent
        border.width: smartCategories.controller.reviewingUncertain ? 1 : 0
        border.color: Theme.warningBorder

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 7
            spacing: 7

            Rectangle {
                width: 18
                height: 18
                radius: 9
                color: Theme.warningSurface
                border.width: 1
                border.color: Theme.warningBorder
                Label {
                    anchors.centerIn: parent
                    text: "?"
                    color: Theme.warningText
                    font.pixelSize: 11
                    font.weight: Font.Bold
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Review uncertain")
                color: smartCategories.workspace.textPrimary
                font.pixelSize: 11
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Label {
                text: smartCategories.controller.uncertainCount
                color: Theme.warningText
                font.pixelSize: 10
            }
        }

        MouseArea {
            id: uncertaintyMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                smartCategories.controller.selectUncertain()
                smartCategories.workspace.commitLibraryScopeSelection()
            }
        }
    }

    ListView {
        id: categoryList
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, 162)
        clip: true
        spacing: 1
        model: smartCategories.controller.categories

        delegate: Rectangle {
            id: categoryRow
            required property var modelData
            readonly property bool selected: Boolean(modelData.selected)
            width: categoryList.width
            height: Boolean(modelData.enabled) ? 30 : 0
            visible: Boolean(modelData.enabled)
            radius: Theme.compactControlRadius
            color: selected ? Theme.accentSurface
                : mouse.containsMouse ? Theme.buttonGhostHover : Theme.transparent

            Rectangle {
                anchors.left: parent.left
                anchors.leftMargin: 3
                anchors.verticalCenter: parent.verticalCenter
                width: 2
                height: 14
                radius: 1
                color: categoryRow.selected
                    ? smartCategories.workspace.accent : Theme.transparent
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 11
                anchors.rightMargin: 7
                spacing: 7
                ShadowIcon {
                    source: "qrc:/icons/filter.svg"
                    color: categoryRow.selected ? smartCategories.workspace.accent
                        : smartCategories.workspace.textMuted
                    size: 13
                }
                Label {
                    Layout.fillWidth: true
                    text: String(categoryRow.modelData.name)
                    color: smartCategories.workspace.textPrimary
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Label {
                    text: String(categoryRow.modelData.count)
                    color: categoryRow.selected ? smartCategories.workspace.accent
                        : smartCategories.workspace.textMuted
                    font.pixelSize: 10
                }
            }
            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (categoryRow.selected)
                        smartCategories.controller.clearSelection()
                    else
                        smartCategories.controller.selectCategory(
                            String(categoryRow.modelData.id))
                    smartCategories.workspace.commitLibraryScopeSelection()
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: smartCategories.controller.busy
            || smartCategories.controller.canResume
            || smartCategories.controller.needsUpdate
            || smartCategories.controller.failed
            || (smartCategories.controller.hasPublishedResults
                && !smartCategories.controller.hasMatches)
        spacing: 5

        Label {
            Layout.fillWidth: true
            text: smartCategories.controller.failed
                ? smartCategories.controller.errorText
                : smartCategories.controller.statusText
            color: smartCategories.controller.failed ? Theme.errorText
                : smartCategories.workspace.textMuted
            font.pixelSize: 9
            elide: Text.ElideRight
        }
        ShadowButton {
            text: smartCategories.controller.busy ? qsTr("Pause")
                : smartCategories.controller.failed ? qsTr("Retry")
                : smartCategories.controller.canResume ? qsTr("Continue")
                : qsTr("Update")
            variant: ShadowButton.Ghost
            enabled: !smartCategories.controller.busy
                || smartCategories.controller.canPause
            onClicked: {
                if (smartCategories.controller.busy)
                    smartCategories.controller.pause()
                else if (smartCategories.controller.canResume)
                    smartCategories.controller.resume()
                else
                    smartCategories.controller.refresh()
            }
        }
    }
}
