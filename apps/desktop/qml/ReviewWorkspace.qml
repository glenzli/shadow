pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: review

    required property var controller
    property string selectedPhotoId: ""
    property string selectedRepresentationId: ""
    property string selectedTitle: ""
    property string selectedPath: ""
    property string selectedRole: ""
    property int selectedWidth: 0
    property int selectedHeight: 0

    signal openPrecisionRequested(string photoId, string representationId,
                                  string sourcePath, string photoTitle)

    readonly property color panel: "#121519"
    readonly property color panelRaised: "#181c21"
    readonly property color border: "#2a3037"
    readonly property color textPrimary: "#edf0f2"
    readonly property color textMuted: "#8b949e"
    readonly property color accent: "#d8b36a"

    function chooseFolder() {
        folderDialog.open()
    }

    function selectPhoto(card) {
        selectedPhotoId = card.photoId
        selectedRepresentationId = card.representationId
        selectedTitle = card.title
        selectedPath = card.sourcePath
        selectedRole = card.visualRole
        selectedWidth = card.visualWidth
        selectedHeight = card.visualHeight
    }

    function clearSelection() {
        selectedPhotoId = ""
        selectedRepresentationId = ""
        selectedTitle = ""
        selectedPath = ""
        selectedRole = ""
        selectedWidth = 0
        selectedHeight = 0
    }

    function openSelectedPhoto() {
        if (selectedPhotoId.length > 0 && selectedRepresentationId.length > 0) {
            openPrecisionRequested(selectedPhotoId, selectedRepresentationId,
                                   selectedPath, selectedTitle)
        }
    }

    FolderDialog {
        id: folderDialog
        title: "Choose a photo folder"
        onAccepted: review.controller.scanFolder(selectedFolder)
    }

    Connections {
        target: review.controller
        function onItemCountChanged() {
            if (review.controller.itemCount === 0)
                review.clearSelection()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 210
            Layout.fillHeight: true
            color: review.panel
            border.color: review.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 8

                Label {
                    text: "LIBRARY"
                    color: review.textMuted
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
                        Label { text: "All Photos"; color: review.textPrimary }
                        Label {
                            Layout.fillWidth: true
                            text: review.controller.itemCount
                            color: review.textMuted
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    topPadding: 10
                    text: "Original files stay read-only. Review uses embedded previews first and rebuildable local proxies when needed."
                    color: review.textMuted
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
                    if (review.controller.hasMore
                            && !review.controller.busy
                            && !review.controller.loadingMore
                            && contentY + height >= contentHeight - cellHeight * 2) {
                        review.controller.loadMore()
                    }
                }

                anchors.fill: parent
                anchors.margins: 18
                clip: true
                model: review.controller.model
                cellWidth: Math.max(220, Math.floor(width / Math.max(1, Math.floor(width / 270))))
                cellHeight: cellWidth * 0.78
                currentIndex: -1
                onContentYChanged: maybeLoadMore()
                onHeightChanged: Qt.callLater(maybeLoadMore)
                onCountChanged: {
                    if (count === 0)
                        currentIndex = -1
                    else if (currentIndex < 0)
                        currentIndex = 0
                    Qt.callLater(maybeLoadMore)
                }
                onCurrentItemChanged: {
                    if (currentItem)
                        review.selectPhoto(currentItem)
                }

                delegate: Item {
                    id: card
                    width: grid.cellWidth
                    height: grid.cellHeight

                    required property int index
                    required property string photoId
                    required property string representationId
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
                        color: review.panelRaised
                        border.width: grid.currentIndex === card.index ? 2 : 1
                        border.color: grid.currentIndex === card.index ? review.accent : review.border

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
                                    color: review.textMuted
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
                                    color: review.textPrimary
                                    elide: Text.ElideRight
                                    font.pixelSize: 11
                                }
                                Label {
                                    text: card.visualWidth > 0
                                        ? card.visualWidth + " × " + card.visualHeight
                                        : "awaiting cache"
                                    color: review.textMuted
                                    font.pixelSize: 9
                                }
                            }

                            Label {
                                id: badge
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.visualRole.length > 0 ? card.visualRole.toUpperCase() : "RAW"
                                color: card.visualRole === "embedded" ? "#9fc7a7" : review.accent
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.8
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                grid.currentIndex = card.index
                                review.selectPhoto(card)
                            }
                            onDoubleClicked: {
                                grid.currentIndex = card.index
                                review.selectPhoto(card)
                                review.openPrecisionRequested(card.photoId, card.representationId,
                                                              card.sourcePath, card.title)
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: Math.min(420, parent.width - 60)
                    visible: grid.count === 0 && !review.controller.busy
                    text: "Choose a folder to scan RAW files.\nShadow will use embedded previews first and generate a local proxy only when needed."
                    color: review.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    lineHeight: 1.4
                }

                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 18
                    visible: review.controller.loadingMore
                    running: visible
                    width: 34
                    height: 34
                }
            }

            Rectangle {
                anchors.fill: parent
                visible: review.controller.busy
                color: "#b00c0e10"

                Column {
                    anchors.centerIn: parent
                    spacing: 14
                    BusyIndicator {
                        anchors.horizontalCenter: parent.horizontalCenter
                        running: true
                    }
                    Label {
                        text: "Building Review previews"
                        color: review.textPrimary
                        font.pixelSize: 14
                    }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 278
            Layout.fillHeight: true
            color: review.panel
            border.color: review.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 10

                Label {
                    text: "PHOTO"
                    color: review.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.6
                }

                Label {
                    Layout.fillWidth: true
                    text: review.selectedTitle.length > 0 ? review.selectedTitle : "Nothing selected"
                    color: review.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: review.selectedPath
                    color: review.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 3
                    elide: Text.ElideMiddle
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: review.border
                }

                GridLayout {
                    columns: 2
                    rowSpacing: 8
                    columnSpacing: 12

                    Label { text: "VISUAL"; color: review.textMuted; font.pixelSize: 9 }
                    Label {
                        text: review.selectedRole.length > 0
                            ? review.selectedRole.toUpperCase()
                            : "PENDING"
                        color: review.textPrimary
                        font.pixelSize: 10
                    }
                    Label { text: "SIZE"; color: review.textMuted; font.pixelSize: 9 }
                    Label {
                        text: review.selectedWidth > 0
                            ? review.selectedWidth + " × " + review.selectedHeight
                            : "—"
                        color: review.textPrimary
                        font.pixelSize: 10
                    }
                }

                Button {
                    id: openButton
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    enabled: review.selectedPhotoId.length > 0
                        && review.selectedRepresentationId.length > 0
                    text: "OPEN IN PRECISION"
                    onClicked: review.openSelectedPhoto()

                    background: Rectangle {
                        radius: 4
                        color: openButton.enabled
                            ? (openButton.down ? "#b9914e" : review.accent)
                            : "#252a30"
                    }
                    contentItem: Label {
                        text: openButton.text
                        color: openButton.enabled ? "#17130d" : "#606a74"
                        font.pixelSize: 10
                        font.weight: Font.Bold
                        font.letterSpacing: 0.6
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Item { Layout.fillHeight: true }

                Label {
                    Layout.fillWidth: true
                    text: "Double-click a photo to enter its non-destructive Precision workspace."
                    color: "#66717c"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    lineHeight: 1.35
                }
            }
        }
    }
}
