pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "lutManagerWindow"

    required property var lutLibrary

    width: 780
    height: 580
    minimumWidth: 640
    minimumHeight: 440
    title: qsTr("LUT Library")
    color: Theme.window
    visible: false
    modality: Qt.NonModal
    palette.window: Theme.window
    palette.windowText: Theme.textPrimary
    palette.base: Theme.panelRaised
    palette.text: Theme.textPrimary
    palette.button: Theme.buttonSurface
    palette.buttonText: Theme.textPrimary
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.selectionForeground

    function openManager() {
        lutLibrary.rescan()
        show()
        raise()
        requestActivate()
    }

    FolderDialog {
        id: lutFolderDialog
        title: qsTr("Choose a LUT folder")
        onAccepted: root.lutLibrary.addDirectory(selectedFolder)
    }

    header: ToolBar {
        implicitHeight: 50
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
        contentItem: RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 12
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: qsTr("LUT LIBRARY")
                    color: Theme.textPrimary
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.0
                }
                Label {
                    text: qsTr("%1 available · %2 need attention")
                        .arg(root.lutLibrary.validCount).arg(root.lutLibrary.errorCount)
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
            }

            ShadowButton {
                text: qsTr("Add Folder")
                variant: ShadowButton.Secondary
                onClicked: lutFolderDialog.open()
            }
            ShadowIconButton {
                source: "qrc:/icons/redo.svg"
                toolTipText: qsTr("Rescan LUT folders")
                accessibleName: toolTipText
                onClicked: root.lutLibrary.rescan()
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 260
            Layout.fillHeight: true
            color: Theme.panel
            border.color: Theme.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 10

                Label {
                    text: qsTr("SOURCE FOLDERS")
                    color: Theme.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.7
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 6
                    model: root.lutLibrary.directories
                    delegate: Rectangle {
                        id: directoryDelegate
                        required property string modelData
                        width: ListView.view.width
                        height: 54
                        radius: Theme.controlRadius
                        color: Theme.surfaceSubtle
                        border.color: Theme.border

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 6
                            spacing: 6
                            Label {
                                Layout.fillWidth: true
                                text: directoryDelegate.modelData
                                color: Theme.textPrimary
                                font.pixelSize: 10
                                elide: Text.ElideMiddle
                                wrapMode: Text.WrapAnywhere
                                maximumLineCount: 2
                            }
                            ShadowIconButton {
                                source: "qrc:/icons/trash.svg"
                                buttonSize: 28
                                toolTipText: qsTr("Remove folder from LUT Library")
                                accessibleName: toolTipText
                                onClicked: root.lutLibrary.removeDirectory(
                                    directoryDelegate.modelData)
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: parent.count === 0
                        width: parent.width - 24
                        text: qsTr("Add one or more folders. Shadow scans subfolders for .cube LUTs.")
                        color: Theme.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.window

            ListView {
                id: lutList
                anchors.fill: parent
                anchors.margins: 14
                clip: true
                spacing: 7
                model: root.lutLibrary.entries
                delegate: Rectangle {
                    id: lutEntry
                    required property var modelData
                    width: ListView.view.width
                    height: modelData.valid ? 88 : 76
                    radius: Theme.controlRadius
                    color: modelData.valid ? Theme.panel : Theme.warningSurface
                    border.color: modelData.valid ? Theme.border : Theme.warningBorder

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: lutEntry.modelData.valid ? 112 : 40
                            Layout.preferredHeight: lutEntry.modelData.valid ? 64 : 40
                            radius: Theme.controlRadius
                            clip: true
                            color: lutEntry.modelData.valid
                                ? Theme.photoCanvas : Theme.warningSurface
                            border.color: lutEntry.modelData.valid
                                ? Theme.border : Theme.warningBorder

                            Image {
                                anchors.fill: parent
                                visible: lutEntry.modelData.valid
                                source: visible
                                    ? "image://shadow-lut/" + lutEntry.modelData.id : ""
                                sourceSize.width: 224
                                sourceSize.height: 128
                                asynchronous: true
                                cache: true
                                fillMode: Image.PreserveAspectCrop
                            }

                            Label {
                                anchors.centerIn: parent
                                visible: !lutEntry.modelData.valid
                                text: "!"
                                color: Theme.warningText
                                font.pixelSize: 12
                                font.weight: Font.Bold
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: lutEntry.modelData.valid
                                    ? lutEntry.modelData.title : lutEntry.modelData.fileName
                                color: Theme.textPrimary
                                font.pixelSize: 12
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: lutEntry.modelData.valid
                                    ? qsTr("%1³ · %2").arg(lutEntry.modelData.size)
                                        .arg(lutEntry.modelData.fileName)
                                    : lutEntry.modelData.error
                                color: lutEntry.modelData.valid
                                    ? Theme.textMuted : Theme.warningText
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: !lutEntry.modelData.valid
                                text: lutEntry.modelData.path
                                color: Theme.textFaint
                                font.pixelSize: 9
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: parent.count === 0
                    width: parent.width - 48
                    text: root.lutLibrary.directories.length === 0
                        ? qsTr("Your LUT Library is empty")
                        : qsTr("No .cube LUTs were found in the configured folders")
                    color: Theme.textMuted
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
