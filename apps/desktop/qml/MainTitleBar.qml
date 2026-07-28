pragma ComponentBehavior: Bound
pragma Translator: "Main"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ToolBar {
    id: titleBar

    required property var hostWindow
    required property var editor
    required property var preferencesMenu
    required property int workspaceIndex
    required property string descriptiveTitle
    required property bool canOpenSelectedPhoto

    signal reviewRequested()
    signal precisionRequested()

    objectName: "titleToolBar"
    Accessible.name: descriptiveTitle
    implicitHeight: 44
    topPadding: 0
    bottomPadding: 0
    leftPadding: Math.max(
        SafeArea.margins.left,
        Qt.platform.os === "osx"
            && hostWindow.visibility !== Window.FullScreen ? 96 : 16
    )
    rightPadding: Math.max(
        SafeArea.margins.right,
        Qt.platform.os === "windows" ? 152 : 16
    )

    Popup {
        id: historyPopup
        parent: Overlay.overlay
        width: Math.min(368, parent.width - 32)
        padding: 0
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        x: Math.max(12, Math.min(parent.width - width - 12,
            historyButton.mapToItem(parent, 0, historyButton.height + 8).x))
        y: historyButton.mapToItem(parent, 0, historyButton.height + 8).y

        background: Rectangle {
            radius: Theme.controlRadius + 2
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 0

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("HISTORY")
                    color: Theme.textPrimary
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.05
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("CURRENT WORKING COPY")
                    color: Theme.accent
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Edits are autosaved to each photo’s current working copy. They remain editable and are not catalog commits.")
                    color: Theme.textSecondary
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                    lineHeight: 1.32
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                spacing: 7

                Label {
                    Layout.fillWidth: true
                    text: qsTr("CATALOG HISTORY")
                    color: Theme.textPrimary
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Named commits, branches, and shared adjustment snapshots will live here at catalog scope, not inside a single photo’s inspector.")
                    color: Theme.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                    lineHeight: 1.32
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("No catalog commits yet")
                    color: Theme.textDisabled
                    font.pixelSize: 10
                }
            }
        }
    }

    background: Rectangle {
        color: Theme.chrome

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.border
        }
    }

    contentItem: Item {
        Item {
            anchors.fill: parent

            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                onActiveChanged: {
                    if (active)
                        titleBar.hostWindow.startSystemMove()
                }
            }
        }

        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Label {
                text: "SHADOW"
                color: Theme.textPrimary
                font.pixelSize: 14
                font.weight: Font.DemiBold
                font.letterSpacing: 2.5
            }

            Rectangle {
                width: 1
                height: 18
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.border
            }
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            height: titleBar.availableHeight
            spacing: 10

            ShadowTabButton {
                height: parent.height
                active: titleBar.workspaceIndex === 0
                iconSource: "qrc:/icons/review-grid.svg"
                iconSize: 18
                minimumTabWidth: 46
                underlineInset: 22
                underlineBottomMargin: -titleBar.bottomPadding
                text: qsTr("REVIEW")
                toolTipText: text
                onClicked: titleBar.reviewRequested()
            }

            ShadowTabButton {
                height: parent.height
                active: titleBar.workspaceIndex === 1
                iconSource: "qrc:/icons/edit.svg"
                iconSize: 18
                minimumTabWidth: 46
                underlineInset: 22
                underlineBottomMargin: -titleBar.bottomPadding
                text: qsTr("PRECISION")
                toolTipText: text
                enabled: titleBar.editor.active || titleBar.editor.busy
                    || titleBar.canOpenSelectedPhoto
                onClicked: titleBar.precisionRequested()
            }
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            Row {
                visible: titleBar.workspaceIndex === 1 && titleBar.editor.active
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Rectangle {
                    width: 7
                    height: 7
                    anchors.verticalCenter: parent.verticalCenter
                    radius: width / 2
                    color: !titleBar.editor.dirty ? Theme.savedText
                        : titleBar.editor.autosaveFailed
                            ? Theme.errorText : Theme.warningText
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: !titleBar.editor.dirty ? qsTr("SAVED")
                        : titleBar.editor.autosaveFailed ? qsTr("SAVE FAILED")
                        : titleBar.editor.autosavePending ? qsTr("SAVING") : qsTr("DRAFT")
                    color: !titleBar.editor.dirty ? Theme.savedText
                        : titleBar.editor.autosaveFailed
                            ? Theme.errorText : Theme.warningText
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.65
                }
            }

            Row {
                visible: titleBar.workspaceIndex === 1
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                ShadowIconButton {
                    source: "qrc:/icons/undo.svg"
                    toolTipText: qsTr("Undo")
                    accessibleName: toolTipText
                    enabled: titleBar.editor.active && titleBar.editor.canUndo
                        && !titleBar.editor.stateBusy
                    onClicked: titleBar.editor.undo()
                }

                ShadowIconButton {
                    source: "qrc:/icons/redo.svg"
                    toolTipText: qsTr("Redo")
                    accessibleName: toolTipText
                    enabled: titleBar.editor.active && titleBar.editor.canRedo
                        && !titleBar.editor.stateBusy
                    onClicked: titleBar.editor.redo()
                }
            }

            ShadowIconButton {
                id: historyButton
                anchors.verticalCenter: parent.verticalCenter
                source: "qrc:/icons/history.svg"
                text: qsTr("History")
                toolTipText: text
                accessibleName: text
                selected: historyPopup.opened
                onClicked: historyPopup.opened
                    ? historyPopup.close() : historyPopup.open()
            }

            ShadowIconButton {
                visible: titleBar.workspaceIndex === 1
                anchors.verticalCenter: parent.verticalCenter
                source: "qrc:/icons/clear.svg"
                text: qsTr("Return to Review")
                toolTipText: text
                accessibleName: text
                onClicked: titleBar.reviewRequested()
            }

            ShadowIconButton {
                id: settingsButton
                objectName: "settingsButton"
                anchors.verticalCenter: parent.verticalCenter
                buttonSize: 28
                source: "qrc:/icons/settings.svg"
                text: qsTr("Settings")
                toolTipText: text
                accessibleName: text
                onClicked: titleBar.preferencesMenu.popup(
                    settingsButton,
                    settingsButton.width - titleBar.preferencesMenu.width,
                    settingsButton.height + titleBar.bottomPadding + 4
                )
            }
        }
    }
}
