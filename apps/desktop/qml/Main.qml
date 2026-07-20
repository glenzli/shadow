pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property var controller
    property string selectedTitle: ""
    property string selectedPath: ""
    property string selectedVisual: ""
    property string selectedRole: ""
    property int selectedWidth: 0
    property int selectedHeight: 0

    function selectPhoto(card) {
        selectedTitle = card.title
        selectedPath = card.sourcePath
        selectedVisual = card.visualSource
        selectedRole = card.visualRole
        selectedWidth = card.visualWidth
        selectedHeight = card.visualHeight
    }

    function clearSelection() {
        selectedTitle = ""
        selectedPath = ""
        selectedVisual = ""
        selectedRole = ""
        selectedWidth = 0
        selectedHeight = 0
    }

    width: 1480
    height: 920
    minimumWidth: 1080
    minimumHeight: 680
    visible: true
    color: "#0c0e10"
    title: "Shadow · Review"

    readonly property color panel: "#121519"
    readonly property color panelRaised: "#181c21"
    readonly property color border: "#2a3037"
    readonly property color textPrimary: "#edf0f2"
    readonly property color textMuted: "#8b949e"
    readonly property color accent: "#d8b36a"

    FolderDialog {
        id: folderDialog
        title: "Choose a photo folder"
        onAccepted: window.controller.scanFolder(selectedFolder)
    }

    Connections {
        target: window.controller
        function onItemCountChanged() {
            if (window.controller.itemCount === 0)
                window.clearSelection()
        }
    }

    Popup {
        id: previewPopup
        anchors.centerIn: Overlay.overlay
        width: Overlay.overlay.width
        height: Overlay.overlay.height
        modal: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle { color: "#ed08090b" }

        contentItem: Item {
            Image {
                anchors.fill: parent
                anchors.margins: 52
                source: window.selectedVisual
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                sourceSize.width: 2200
                sourceSize.height: 1600
            }

            ToolButton {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 22
                text: "CLOSE"
                onClicked: previewPopup.close()
            }
        }
    }

    header: Rectangle {
        height: 58
        color: "#101317"
        border.color: window.border

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 16
            spacing: 18

            Label {
                text: "SHADOW"
                color: window.textPrimary
                font.pixelSize: 17
                font.weight: Font.DemiBold
                font.letterSpacing: 3.2
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 24
                color: window.border
            }

            Label {
                text: "REVIEW"
                color: window.accent
                font.pixelSize: 12
                font.weight: Font.DemiBold
                font.letterSpacing: 1.5
            }

            Label {
                Layout.fillWidth: true
                text: window.controller.folderPath.length > 0
                    ? window.controller.folderPath
                    : "No library folder selected"
                color: window.textMuted
                elide: Text.ElideMiddle
                horizontalAlignment: Text.AlignHCenter
            }

            Button {
                id: folderButton
                text: window.controller.busy ? "SCANNING…" : "CHOOSE FOLDER"
                enabled: !window.controller.busy && !window.controller.loadingMore
                onClicked: folderDialog.open()

                background: Rectangle {
                    radius: 4
                    color: folderButton.down ? "#b9914e" : window.accent
                }
                contentItem: Label {
                    text: folderButton.text
                    color: "#17130d"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 210
            Layout.fillHeight: true
            color: window.panel
            border.color: window.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 8

                Label {
                    text: "LIBRARY"
                    color: window.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.6
                }

                Item { Layout.preferredHeight: 6 }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    radius: 4
                    color: "#22272d"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 10
                        Label { text: "All Photos"; color: window.textPrimary }
                        Label {
                            Layout.fillWidth: true
                            text: window.controller.itemCount
                            color: window.textMuted
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    topPadding: 10
                    text: "The first Review slice keeps original files read-only and stores only rebuildable previews."
                    color: window.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    lineHeight: 1.35
                }

                Item { Layout.fillHeight: true }

                Label {
                    text: "LOCAL · MACOS"
                    color: "#64707b"
                    font.pixelSize: 9
                    font.letterSpacing: 1.2
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#0c0e10"

            GridView {
                id: grid

                function maybeLoadMore() {
                    if (window.controller.hasMore
                            && !window.controller.busy
                            && !window.controller.loadingMore
                            && contentY + height >= contentHeight - cellHeight * 2)
                        window.controller.loadMore()
                }

                anchors.fill: parent
                anchors.margins: 18
                clip: true
                model: window.controller.model
                cellWidth: Math.max(220, Math.floor(width / Math.max(1, Math.floor(width / 270))))
                cellHeight: cellWidth * 0.78
                currentIndex: -1
                onContentYChanged: maybeLoadMore()
                onHeightChanged: Qt.callLater(maybeLoadMore)
                onCountChanged: {
                    if (count === 0) {
                        currentIndex = -1
                    } else if (currentIndex < 0) {
                        currentIndex = 0
                    }
                    Qt.callLater(maybeLoadMore)
                }
                onCurrentItemChanged: {
                    if (currentItem)
                        window.selectPhoto(currentItem)
                }

                delegate: Item {
                    id: card
                    width: grid.cellWidth
                    height: grid.cellHeight

                    required property int index
                    required property string title
                    required property string sourcePath
                    required property string visualRole
                    required property string visualError
                    required property int visualWidth
                    required property int visualHeight
                    required property string visualSource

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 5
                        radius: 5
                        color: window.panelRaised
                        border.width: grid.currentIndex === card.index ? 2 : 1
                        border.color: grid.currentIndex === card.index ? window.accent : window.border

                        Image {
                            id: thumbnail
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: caption.top
                            source: card.visualSource
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: true
                            sourceSize.width: 640
                            sourceSize.height: 480
                        }

                        Rectangle {
                            anchors.fill: thumbnail
                            visible: card.visualSource.length === 0
                            color: "#20252b"

                            Column {
                                anchors.centerIn: parent
                                spacing: 8
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "RAW"
                                    color: "#727d88"
                                    font.pixelSize: 20
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 2
                                }
                                Label {
                                    text: card.visualError.length > 0 ? "PREVIEW PENDING" : "NO VISUAL"
                                    color: window.textMuted
                                    font.pixelSize: 9
                                }
                            }
                        }

                        Rectangle {
                            id: caption
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 52
                            color: "#e615181c"

                            Column {
                                anchors.left: parent.left
                                anchors.right: badge.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 10
                                spacing: 3
                                Label {
                                    width: parent.width
                                    text: card.title
                                    color: window.textPrimary
                                    elide: Text.ElideRight
                                    font.pixelSize: 11
                                }
                                Label {
                                    text: card.visualWidth > 0
                                        ? card.visualWidth + " × " + card.visualHeight
                                        : "awaiting cache"
                                    color: window.textMuted
                                    font.pixelSize: 9
                                }
                            }

                            Label {
                                id: badge
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.visualRole.length > 0 ? card.visualRole.toUpperCase() : "RAW"
                                color: card.visualRole === "embedded" ? "#9fc7a7" : window.accent
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.8
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                grid.currentIndex = card.index
                                window.selectPhoto(card)
                            }
                            onDoubleClicked: {
                                grid.currentIndex = card.index
                                window.selectPhoto(card)
                                if (card.visualSource.length > 0)
                                    previewPopup.open()
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: Math.min(420, parent.width - 60)
                    visible: grid.count === 0 && !window.controller.busy
                    text: "Choose a folder to scan RAW files.\nShadow will use embedded previews first and generate a local proxy only when needed."
                    color: window.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    lineHeight: 1.4
                }


                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 18
                    visible: window.controller.loadingMore
                    running: visible
                    width: 34
                    height: 34
                }
            }

            Rectangle {
                anchors.fill: parent
                visible: window.controller.busy
                color: "#b00c0e10"

                Column {
                    anchors.centerIn: parent
                    spacing: 14
                    BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: true }
                    Label {
                        text: "Building Review previews"
                        color: window.textPrimary
                        font.pixelSize: 14
                    }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 278
            Layout.fillHeight: true
            color: window.panel
            border.color: window.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 10

                Label {
                    text: "PHOTO"
                    color: window.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.6
                }

                Label {
                    Layout.fillWidth: true
                    text: window.selectedTitle.length > 0 ? window.selectedTitle : "Nothing selected"
                    color: window.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: window.selectedPath
                    color: window.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 3
                    elide: Text.ElideMiddle
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: window.border
                }

                GridLayout {
                    columns: 2
                    rowSpacing: 8
                    columnSpacing: 12

                    Label { text: "VISUAL"; color: window.textMuted; font.pixelSize: 9 }
                    Label {
                        text: window.selectedRole.length > 0
                            ? window.selectedRole.toUpperCase()
                            : "PENDING"
                        color: window.textPrimary
                        font.pixelSize: 10
                    }
                    Label { text: "SIZE"; color: window.textMuted; font.pixelSize: 9 }
                    Label {
                        text: window.selectedWidth > 0
                            ? window.selectedWidth + " × " + window.selectedHeight
                            : "—"
                        color: window.textPrimary
                        font.pixelSize: 10
                    }
                }

                Item { Layout.fillHeight: true }

                Label {
                    Layout.fillWidth: true
                    text: "AI review and editing controls arrive after the real grid interaction is stable."
                    color: "#66717c"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    lineHeight: 1.35
                }
            }
        }
    }

    footer: Rectangle {
        height: 30
        color: "#101317"
        border.color: window.border

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            Label {
                Layout.fillWidth: true
                text: window.controller.statusText
                color: window.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
            }
            Label {
                text: "CORE v0.1"
                color: "#56616b"
                font.pixelSize: 9
                font.letterSpacing: 0.8
            }
        }
    }
}
