pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: sidebar

    required property var workspace
    required property var albumDialogs

    Layout.preferredWidth: 210
    Layout.fillHeight: true
    color: sidebar.workspace.panel

    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 1
        color: sidebar.workspace.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.panelPadding
        anchors.rightMargin: Theme.panelPadding + 1
        anchors.topMargin: Theme.panelPadding
        anchors.bottomMargin: Theme.panelPadding
        spacing: 8

        Label {
            text: qsTranslate("ReviewWorkspace", "LIBRARY")
            color: sidebar.workspace.textMuted
            font.pixelSize: 10
            font.weight: Font.DemiBold
            font.letterSpacing: 1.6
        }

        Item { Layout.preferredHeight: 6 }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            radius: 7
            color: sidebar.workspace.isSystemCollectionActive("all")
                ? Theme.accentSurface : Theme.transparent

            Rectangle {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 3
                width: 3
                height: 18
                radius: 1.5
                color: sidebar.workspace.isSystemCollectionActive("all")
                    ? sidebar.workspace.accent : Theme.transparent
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 10
                Label { text: qsTranslate("ReviewWorkspace", "All Photos"); color: sidebar.workspace.textPrimary }
                Label {
                    Layout.fillWidth: true
                    text: qsTranslate("ReviewWorkspace", "%L1").arg(sidebar.workspace.controller.itemCount)
                    color: sidebar.workspace.textMuted
                    horizontalAlignment: Text.AlignRight
                }
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: sidebar.workspace.applySystemCollection("all")
            }
        }

        Repeater {
            model: [
                {
                    id: "recent-imports",
                    title: qsTranslate("ReviewWorkspace", "Recent Imports"),
                    icon: "qrc:/icons/history.svg",
                    enabled: false,
                    hint: qsTranslate("ReviewWorkspace", "Recent import sessions will appear here when import-time filtering is available.")
                },
                {
                    id: "liked",
                    title: qsTranslate("ReviewWorkspace", "Liked"),
                    icon: "qrc:/icons/heart.svg",
                    enabled: true,
                    hint: qsTranslate("ReviewWorkspace", "Show photos marked Like")
                },
                {
                    id: "five-star",
                    title: qsTranslate("ReviewWorkspace", "5 Stars"),
                    icon: "qrc:/icons/star.svg",
                    enabled: true,
                    hint: qsTranslate("ReviewWorkspace", "Show photos rated 5 stars")
                }
            ]

            delegate: Rectangle {
                id: defaultCollectionRow
                required property var modelData
                readonly property bool selected:
                    sidebar.workspace.isSystemCollectionActive(String(modelData.id))
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                radius: Theme.compactControlRadius
                color: selected ? Theme.accentSurface
                    : defaultCollectionMouse.containsMouse && modelData.enabled
                        ? Theme.buttonGhostHover : Theme.transparent

                Rectangle {
                    anchors.left: parent.left
                    anchors.leftMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    width: 2
                    height: 16
                    radius: 1
                    color: defaultCollectionRow.selected
                        ? sidebar.workspace.accent : Theme.transparent
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 13
                    anchors.rightMargin: 9
                    spacing: 7

                    ShadowIcon {
                        source: String(defaultCollectionRow.modelData.icon)
                        color: defaultCollectionRow.modelData.enabled
                            ? (defaultCollectionRow.selected
                                ? sidebar.workspace.accent : sidebar.workspace.textMuted)
                            : Theme.textDisabled
                        size: 14
                    }

                    Label {
                        Layout.fillWidth: true
                        text: String(defaultCollectionRow.modelData.title)
                        color: defaultCollectionRow.modelData.enabled
                            ? (defaultCollectionRow.selected
                                ? sidebar.workspace.textPrimary : sidebar.workspace.textSecondary)
                            : Theme.textDisabled
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }

                    Label {
                        visible: !defaultCollectionRow.modelData.enabled
                        text: qsTranslate("ReviewWorkspace", "SOON")
                        color: Theme.textDisabled
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.5
                    }
                }

                MouseArea {
                    id: defaultCollectionMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: defaultCollectionRow.modelData.enabled
                        ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: {
                        if (defaultCollectionRow.modelData.enabled) {
                            sidebar.workspace.applySystemCollection(
                                String(defaultCollectionRow.modelData.id))
                        }
                    }
                }

                ToolTip {
                    parent: defaultCollectionRow
                    visible: defaultCollectionMouse.containsMouse
                        && !defaultCollectionRow.modelData.enabled
                    delay: 400
                    text: String(defaultCollectionRow.modelData.hint)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 12
            Layout.bottomMargin: 2
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: qsTranslate("ReviewWorkspace", "ALBUMS")
                color: sidebar.workspace.textMuted
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 1.1
            }

            BusyIndicator {
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                visible: sidebar.workspace.controller.libraryAlbumsBusy
                running: visible
            }

            ShadowIconButton {
                source: "qrc:/icons/node-add.svg"
                buttonSize: 24
                iconSize: 15
                toolTipText: qsTranslate("ReviewWorkspace", "Create album")
                accessibleName: toolTipText
                enabled: !sidebar.workspace.controller.libraryAlbumsBusy
                onClicked: sidebar.albumDialogs.openCreate()
            }
        }

        ListView {
            id: albumList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 188)
            visible: count > 0
            clip: true
            spacing: 2
            model: sidebar.workspace.controller.libraryAlbums

            delegate: Rectangle {
                id: albumRow
                required property var modelData
                readonly property string albumId: String(modelData.id)
                readonly property bool selected:
                    sidebar.workspace.controller.libraryAlbumId === albumId
                width: albumList.width
                height: 32
                radius: Theme.compactControlRadius
                color: selected ? Theme.accentSurface
                    : albumMouse.containsMouse
                        ? Theme.buttonGhostHover : Theme.transparent

                Rectangle {
                    anchors.left: parent.left
                    anchors.leftMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    width: 2
                    height: 16
                    radius: 1
                    color: albumRow.selected ? sidebar.workspace.accent : Theme.transparent
                }

                RowLayout {
                    z: 1
                    anchors.fill: parent
                    anchors.leftMargin: 11
                    anchors.rightMargin: 8
                    spacing: 7

                    ShadowIcon {
                        source: String(albumRow.modelData.kind) === "smart"
                            ? "qrc:/icons/filter.svg"
                            : "qrc:/icons/library-manage.svg"
                        color: albumRow.selected ? sidebar.workspace.accent
                            : sidebar.workspace.textMuted
                        size: 14
                    }

                    Label {
                        Layout.fillWidth: true
                        text: String(albumRow.modelData.name)
                        color: albumRow.selected
                            ? sidebar.workspace.textPrimary : sidebar.workspace.textSecondary
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }

                    Label {
                        visible: String(albumRow.modelData.kind) === "smart"
                        text: qsTranslate("ReviewWorkspace", "CONDITION")
                        color: albumRow.selected ? sidebar.workspace.accent : sidebar.workspace.textMuted
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.55
                    }

                    ShadowIconButton {
                        visible: albumRow.selected || albumMouse.containsMouse
                        source: "qrc:/icons/settings.svg"
                        buttonSize: 22
                        iconSize: 13
                        toolTipText: qsTranslate("ReviewWorkspace", "Manage album")
                        accessibleName: toolTipText
                        enabled: !sidebar.workspace.controller.libraryAlbumsBusy
                        onClicked: sidebar.albumDialogs.openManage(
                            albumRow.albumId,
                            String(albumRow.modelData.name),
                            String(albumRow.modelData.kind))
                    }
                }

                MouseArea {
                    id: albumMouse
                    anchors.fill: parent
                    z: 0
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.workspace.controller.libraryAlbumId = albumRow.albumId
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: importStatusColumn.implicitHeight + 20
            visible: sidebar.workspace.controller.scanning
                || (sidebar.workspace.controller.refreshing
                    && Number(sidebar.workspace.controller.scanProgress.scanId) > 0)
            radius: 7
            color: Theme.panelInset
            border.color: Theme.borderStrong

            ColumnLayout {
                id: importStatusColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 5

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: sidebar.workspace.controller.refreshing
                                && !sidebar.workspace.controller.scanning
                            ? qsTranslate("ReviewWorkspace", "LIBRARY REFRESH")
                            : sidebar.workspace.controller.scanProgress.phase === "cancelling"
                            ? qsTranslate("ReviewWorkspace", "STOPPING IMPORT") : qsTranslate("ReviewWorkspace", "IMPORTING")
                        color: sidebar.workspace.accent
                        font.pixelSize: 8
                        font.weight: Font.Bold
                        font.letterSpacing: 0.9
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTranslate("ReviewWorkspace", "%L1 catalogued").arg(
                            sidebar.workspace.controller.scanProgress.cataloguedFiles)
                        color: sidebar.workspace.textPrimary
                        horizontalAlignment: Text.AlignRight
                        font.pixelSize: 9
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: sidebar.workspace.controller.scanning
                        ? qsTranslate("ReviewWorkspace", "%L1 supported · %L2 preview checks queued · %L3 filesystem issues")
                            .arg(sidebar.workspace.controller.scanProgress.supportedFiles)
                            .arg(sidebar.workspace.controller.scanProgress.decodeQueued)
                            .arg(sidebar.workspace.controller.scanProgress.issueCount)
                        : qsTranslate("ReviewWorkspace", "%L1 supported · %L2/%L3 preview checks completed · %L4 filesystem issues")
                            .arg(sidebar.workspace.controller.scanProgress.supportedFiles)
                            .arg(sidebar.workspace.controller.scanProgress.decodeCompleted)
                            .arg(sidebar.workspace.controller.scanProgress.decodeQueued)
                            .arg(sidebar.workspace.controller.scanProgress.issueCount)
                    color: sidebar.workspace.textMuted
                    elide: Text.ElideRight
                    font.pixelSize: 9
                }

                Label {
                    Layout.fillWidth: true
                    visible: !sidebar.workspace.controller.scanning
                    text: qsTranslate("ReviewWorkspace", "%L1 decode failures · %L2 preview failures · %L3 cancelled")
                        .arg(sidebar.workspace.controller.scanProgress.decodeHardFailures)
                        .arg(sidebar.workspace.controller.scanProgress.previewFailures)
                        .arg(sidebar.workspace.controller.scanProgress.decodeCancelled)
                    color: sidebar.workspace.textMuted
                    elide: Text.ElideRight
                    font.pixelSize: 9
                }

                ProgressBar {
                    Layout.fillWidth: true
                    indeterminate: true
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            Layout.topMargin: 10
            visible: sidebar.workspace.controller.sessionEvidenceCount > 0
                || sidebar.workspace.controller.canUndoComparison
            color: sidebar.workspace.border
        }

        RowLayout {
            Layout.fillWidth: true
            visible: sidebar.workspace.controller.sessionEvidenceCount > 0
                || sidebar.workspace.controller.canUndoComparison
            spacing: 6

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: qsTranslate("ReviewWorkspace", "COMPARE EVIDENCE")
                    color: sidebar.workspace.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTranslate("ReviewWorkspace", "%L1 active this session").arg(
                        sidebar.workspace.controller.sessionEvidenceCount)
                    color: sidebar.workspace.textPrimary
                    font.pixelSize: 11
                }
            }

            ShadowIconButton {
                id: undoComparisonButton
                source: "qrc:/icons/undo.svg"
                toolTipText: qsTranslate("ReviewWorkspace", "Forget last comparison")
                accessibleName: toolTipText
                visible: enabled
                enabled: sidebar.workspace.controller.canUndoComparison
                    && !sidebar.workspace.controller.comparisonBusy
                    && !sidebar.workspace.controller.decisionBusy
                    && !sidebar.workspace.controller.scanning
                    && !sidebar.workspace.controller.refreshing
                    && !sidebar.workspace.controller.busy
                    && !sidebar.workspace.controller.loadingMore
                onClicked: sidebar.workspace.controller.undoLastComparison()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !sidebar.workspace.comparison.compareMode
                && (sidebar.workspace.controller.comparisonStatusText.length > 0
                    || sidebar.workspace.comparison.localComparisonStatus.length > 0)
            text: sidebar.workspace.comparison.localComparisonStatus.length > 0
                ? sidebar.workspace.comparison.localComparisonStatus
                : sidebar.workspace.controller.comparisonStatusText
            color: sidebar.workspace.controller.comparisonBusy
                ? sidebar.workspace.accent : sidebar.workspace.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: 9
        }

        Item { Layout.fillHeight: true }

        Label {
            text: qsTranslate("ReviewWorkspace", "LOCAL · MACOS")
            color: Theme.textQuiet
            font.pixelSize: 9
            font.letterSpacing: 1.2
        }
    }
}
