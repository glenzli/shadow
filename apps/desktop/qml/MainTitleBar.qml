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
    required property var exportController
    required property var settingsDialog
    required property var personalProfile
    required property var personalProfileDialog
    required property int workspaceIndex
    required property int reviewWorkspaceIndex
    required property int precisionWorkspaceIndex
    required property int mapWorkspaceIndex
    required property int peopleWorkspaceIndex
    required property string descriptiveTitle
    required property bool canOpenSelectedPhoto
    required property bool historyOpen

    signal reviewRequested()
    signal precisionRequested()
    signal mapRequested()
    signal peopleRequested()
    signal historyRequested()
    signal exportActivityRequested()

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
            id: branding
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Label {
                text: "SHADOW"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSubheading
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

        Item {
            id: navigationArea
            anchors.left: branding.right
            anchors.right: windowActions.left
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            height: parent.height

            Row {
                anchors.centerIn: parent
                height: titleBar.availableHeight
                spacing: 10

                ShadowTabButton {
                    height: parent.height
                    active: titleBar.workspaceIndex
                        === titleBar.reviewWorkspaceIndex
                    iconSource: "qrc:/icons/review-grid.svg"
                    iconSize: 18
                    showLabelWithIcon: navigationArea.width >= 410
                    minimumTabWidth: 46
                    underlineInset: 22
                    underlineBottomMargin: -titleBar.bottomPadding
                    text: qsTr("REVIEW")
                    toolTipText: text
                    onClicked: titleBar.reviewRequested()
                }

                ShadowTabButton {
                    height: parent.height
                    active: titleBar.workspaceIndex
                        === titleBar.precisionWorkspaceIndex
                    iconSource: "qrc:/icons/edit.svg"
                    iconSize: 18
                    showLabelWithIcon: navigationArea.width >= 410
                    minimumTabWidth: 46
                    underlineInset: 22
                    underlineBottomMargin: -titleBar.bottomPadding
                    text: qsTr("PRECISION")
                    toolTipText: text
                    enabled: titleBar.editor.active || titleBar.editor.busy
                        || titleBar.canOpenSelectedPhoto
                    onClicked: titleBar.precisionRequested()
                }

                ShadowTabButton {
                    height: parent.height
                    active: titleBar.workspaceIndex
                        === titleBar.mapWorkspaceIndex
                    iconSource: "qrc:/icons/map.svg"
                    iconSize: 18
                    showLabelWithIcon: navigationArea.width >= 410
                    minimumTabWidth: 46
                    underlineInset: 22
                    underlineBottomMargin: -titleBar.bottomPadding
                    text: qsTr("MAP")
                    toolTipText: text
                    onClicked: titleBar.mapRequested()
                }

                ShadowTabButton {
                    height: parent.height
                    active: titleBar.workspaceIndex
                        === titleBar.peopleWorkspaceIndex
                    iconSource: "qrc:/icons/people.svg"
                    iconSize: 18
                    showLabelWithIcon: navigationArea.width >= 410
                    minimumTabWidth: 46
                    underlineInset: 22
                    underlineBottomMargin: -titleBar.bottomPadding
                    text: qsTr("PEOPLE")
                    toolTipText: text
                    onClicked: titleBar.peopleRequested()
                }
            }
        }

        Row {
            id: windowActions
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            Row {
                visible: titleBar.workspaceIndex
                    === titleBar.precisionWorkspaceIndex
                    && titleBar.editor.active
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
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.65
                }
            }

            Row {
                visible: titleBar.workspaceIndex
                    === titleBar.precisionWorkspaceIndex
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

            ExportActivityButton {
                anchors.verticalCenter: parent.verticalCenter
                exportController: titleBar.exportController
                onClicked: titleBar.exportActivityRequested()
            }

            ShadowIconButton {
                id: historyButton
                anchors.verticalCenter: parent.verticalCenter
                source: "qrc:/icons/history.svg"
                text: qsTr("History")
                toolTipText: text
                accessibleName: text
                selected: titleBar.historyOpen
                onClicked: titleBar.historyRequested()
            }

            ShadowIconButton {
                visible: titleBar.workspaceIndex
                    === titleBar.precisionWorkspaceIndex
                anchors.verticalCenter: parent.verticalCenter
                source: "qrc:/icons/back-to-library.svg"
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
                onClicked: titleBar.settingsDialog.present("general")
            }

            ShadowAvatarButton {
                objectName: "personalProfileButton"
                anchors.verticalCenter: parent.verticalCenter
                profile: titleBar.personalProfile
                onClicked: titleBar.personalProfileDialog.present()
            }
        }
    }
}
