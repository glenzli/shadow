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

    readonly property var browserGroups: buildBrowserGroups(lutLibrary.entries)

    function lastPathSegment(path) {
        const parts = String(path).split("/")
        return parts.length > 0 ? parts[parts.length - 1] : String(path)
    }

    function relativeDirectory(entry) {
        const sourceRoot = String(entry.directory || "")
        const absolutePath = String(entry.path || "")
        const prefix = sourceRoot.length > 0 ? sourceRoot + "/" : ""
        const relativePath = absolutePath.indexOf(prefix) === 0
            ? absolutePath.slice(prefix.length) : String(entry.fileName || "")
        const slash = relativePath.lastIndexOf("/")
        return slash > 0 ? relativePath.slice(0, slash) : ""
    }

    function buildBrowserGroups(entries) {
        const groupsByPath = ({})
        const invalidEntries = []
        for (let index = 0; index < entries.length; ++index) {
            const entry = entries[index]
            if (!entry.valid) {
                invalidEntries.push(entry)
                continue
            }

            const sourceRoot = String(entry.directory || "")
            const relativePath = relativeDirectory(entry)
            const key = sourceRoot + "\u001f" + relativePath
            if (!groupsByPath[key]) {
                groupsByPath[key] = {
                    title: relativePath.length > 0
                        ? lastPathSegment(sourceRoot) + " / " + relativePath
                        : lastPathSegment(sourceRoot),
                    entries: []
                }
            }
            groupsByPath[key].entries.push(entry)
        }

        const groups = []
        for (const key in groupsByPath)
            groups.push(groupsByPath[key])
        groups.sort((left, right) => left.title.localeCompare(right.title))
        if (invalidEntries.length > 0)
            groups.push({ title: qsTr("Needs attention"), entries: invalidEntries, invalid: true })
        return groups
    }

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
                spacing: 16
                model: root.browserGroups
                delegate: ColumnLayout {
                    id: lutGroup
                    required property var modelData
                    width: ListView.view.width
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: lutGroup.modelData.title
                            color: lutGroup.modelData.invalid
                                ? Theme.warningText : Theme.textSecondary
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.55
                            elide: Text.ElideRight
                        }

                        Label {
                            text: qsTr("%1 LUTs").arg(lutGroup.modelData.entries.length)
                            color: Theme.textMuted
                            font.pixelSize: 9
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: width >= 520 ? 3 : 2
                        columnSpacing: 8
                        rowSpacing: 8

                        Repeater {
                            model: lutGroup.modelData.entries
                            delegate: Rectangle {
                                id: lutEntry
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.preferredHeight: lutEntry.modelData.valid ? 148 : 74
                                radius: Theme.controlRadius
                                color: lutEntry.modelData.valid
                                    ? Theme.panel : Theme.warningSurface
                                border.color: lutEntry.modelData.valid
                                    ? Theme.border : Theme.warningBorder

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 6

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: lutEntry.modelData.valid ? 88 : 0
                                        visible: lutEntry.modelData.valid
                                        radius: Theme.compactControlRadius
                                        clip: true
                                        color: Theme.photoCanvas
                                        border.color: Theme.border

                                        Image {
                                            anchors.fill: parent
                                            source: "image://shadow-lut/" + lutEntry.modelData.id
                                            sourceSize.width: 300
                                            sourceSize.height: 176
                                            asynchronous: true
                                            cache: true
                                            fillMode: Image.PreserveAspectCrop
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6

                                        Label {
                                            Layout.fillWidth: true
                                            text: lutEntry.modelData.valid
                                                ? lutEntry.modelData.title
                                                : lutEntry.modelData.fileName
                                            color: Theme.textPrimary
                                            font.pixelSize: 11
                                            font.weight: Font.Medium
                                            elide: Text.ElideRight
                                        }

                                        Label {
                                            visible: !lutEntry.modelData.valid
                                            text: "!"
                                            color: Theme.warningText
                                            font.pixelSize: 11
                                            font.weight: Font.Bold
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: lutEntry.modelData.valid
                                            ? qsTr("%1³ · %2").arg(lutEntry.modelData.size)
                                                .arg(lutEntry.modelData.fileName)
                                            : lutEntry.modelData.error
                                        color: lutEntry.modelData.valid
                                            ? Theme.textMuted : Theme.warningText
                                        font.pixelSize: 9
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
