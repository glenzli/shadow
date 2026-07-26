pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: library

    required property var controller
    signal chooseFolderRequested()

    property string pendingRelinkLocationId: ""
    property url pendingRelinkCandidate: ""

    readonly property bool activityRunning: controller.scanning
        || controller.refreshing || controller.busy

    FileDialog {
        id: relinkFileDialog
        title: qsTr("Choose the moved original file")
        fileMode: FileDialog.OpenFile
        onAccepted: {
            library.pendingRelinkCandidate = selectedFile
            relinkConfirmPopup.open()
        }
    }

    Popup {
        id: relinkConfirmPopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: 390
        padding: 18
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 10
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: qsTr("Verify and link original")
                color: Theme.textPrimary
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow will read the complete selected file and link it only when its exact content identity belongs to this historical photo. File name, EXIF and size are not used as a match.")
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: library.pendingRelinkCandidate.toString()
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideMiddle
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item { Layout.fillWidth: true }

                ShadowButton {
                    text: qsTr("CANCEL")
                    variant: ShadowButton.Ghost
                    onClicked: relinkConfirmPopup.close()
                }

                ShadowButton {
                    text: qsTr("VERIFY AND LINK")
                    variant: ShadowButton.Primary
                    onClicked: {
                        library.controller.relinkMissingSourceLocation(
                            library.pendingRelinkLocationId,
                            library.pendingRelinkCandidate
                        )
                        relinkConfirmPopup.close()
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.window
    }

    ScrollView {
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: Math.min(760, parent.width - 64)
            x: Math.round((parent.width - width) / 2)
            spacing: 18

            Item { Layout.preferredHeight: 32 }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    radius: 8
                    color: Theme.accentSurface

                    ShadowIcon {
                        anchors.centerIn: parent
                        source: "qrc:/icons/library-manage.svg"
                        color: Theme.accent
                        size: 20
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Library Management")
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Manage local photo sources. Original files remain read-only.")
                        color: Theme.textMuted
                        font.pixelSize: 11
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            RowLayout {
                Layout.fillWidth: true

                Label {
                    text: qsTr("SOURCE HEALTH")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.5
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    visible: library.controller.librarySourceHealthBusy
                    running: visible
                }

                ShadowIconButton {
                    visible: !library.controller.librarySourceHealthBusy
                    source: "qrc:/icons/history.svg"
                    variant: ShadowIconButton.Quiet
                    toolTipText: qsTr("REFRESH SOURCE HEALTH")
                    accessibleName: toolTipText
                    onClicked: library.controller.refreshLibrarySourceHealth()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Repeater {
                    model: library.controller.librarySourceHealth

                    delegate: Rectangle {
                        required property var modelData
                        readonly property var sourceHealth: modelData

                        Layout.fillWidth: true
                        implicitHeight: sourceHealthContent.implicitHeight + 28
                        radius: 8
                        color: Theme.panelRaised
                        border.color: Theme.border

                        ColumnLayout {
                            id: sourceHealthContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                ShadowIcon {
                                    source: "qrc:/icons/add-folder.svg"
                                    color: modelData.enabled ? Theme.textMuted : Theme.textSubtle
                                    size: 16
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.sourcePath
                                    color: modelData.enabled ? Theme.textPrimary : Theme.textMuted
                                    font.pixelSize: Theme.fontSection
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideMiddle
                                }

                                Label {
                                    text: modelData.enabled ? qsTr("ACTIVE") : qsTr("PAUSED")
                                    color: modelData.enabled ? Theme.accent : Theme.textSubtle
                                    font.pixelSize: Theme.fontMeta
                                    font.weight: Font.DemiBold
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: !modelData.hasLatestCompletedScan
                                    ? qsTr("No completed scan has been recorded yet.")
                                    : Number(modelData.notSeenLocations) === 0
                                        ? qsTr("The latest scan accounted for all known locations.")
                                        : qsTr("%L1 locations were not seen in this scan.")
                                            .arg(Number(modelData.notSeenLocations).toLocaleString())
                                color: Number(modelData.notSeenLocations) > 0
                                    ? Theme.textSecondary : Theme.textMuted
                                font.pixelSize: Theme.fontMeta
                                wrapMode: Text.WordWrap
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                visible: modelData.hasLatestCompletedScan
                                spacing: 18

                                Label {
                                    text: qsTr("KNOWN  %L1")
                                        .arg(Number(modelData.knownLocations).toLocaleString())
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontMeta
                                }

                                Label {
                                    text: qsTr("SEEN  %L1")
                                        .arg(Number(modelData.seenLocations).toLocaleString())
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontMeta
                                }

                                Item { Layout.fillWidth: true }
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: modelData.hasLatestCompletedScan
                                    && Number(modelData.notSeenLocations) > 0
                                text: qsTr("This is scan evidence for this source only; photos may remain available elsewhere.")
                                color: Theme.textSubtle
                                font.pixelSize: Theme.fontMeta
                                wrapMode: Text.WordWrap
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                visible: modelData.hasLatestCompletedScan
                                    && Number(modelData.notSeenLocations) > 0

                                Item { Layout.fillWidth: true }

                                ShadowIconButton {
                                    visible: library.controller.missingSourceLocationScanId
                                        !== sourceHealth.scanSessionId
                                    source: "qrc:/icons/metadata.svg"
                                    variant: ShadowIconButton.Quiet
                                    toolTipText: qsTr("REVIEW NOT-SEEN LOCATIONS")
                                    accessibleName: toolTipText
                                    onClicked: library.controller.openMissingSourceLocationReview(
                                        sourceHealth.scanSessionId
                                    )
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                visible: library.controller.missingSourceLocationScanId
                                    === sourceHealth.scanSessionId
                                implicitHeight: missingLocationContent.implicitHeight + 20
                                radius: Theme.controlRadius
                                color: Theme.surfaceSubtle

                                ColumnLayout {
                                    id: missingLocationContent
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    spacing: 8

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("NOT-SEEN LOCATIONS")
                                            color: Theme.textSecondary
                                            font.pixelSize: Theme.fontMeta
                                            font.weight: Font.DemiBold
                                            font.letterSpacing: 0.4
                                        }

                                        BusyIndicator {
                                            Layout.preferredWidth: 16
                                            Layout.preferredHeight: 16
                                            visible: library.controller.missingSourceLocationsBusy
                                            running: visible
                                        }

                                        ShadowIconButton {
                                            source: "qrc:/icons/clear.svg"
                                            variant: ShadowIconButton.Quiet
                                            toolTipText: qsTr("CLOSE LOCATION REVIEW")
                                            accessibleName: toolTipText
                                            onClicked: library.controller.closeMissingSourceLocationReview()
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: library.controller.missingSourceLocations.length === 0
                                            && !library.controller.missingSourceLocationsBusy
                                        text: qsTr("No locations are available for this completed scan.")
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.fontMeta
                                        wrapMode: Text.WordWrap
                                    }

                                    Repeater {
                                        model: library.controller.missingSourceLocations

                                        delegate: ColumnLayout {
                                            required property var modelData

                                            Layout.fillWidth: true
                                            spacing: 2

                                            Label {
                                                Layout.fillWidth: true
                                                text: modelData.title
                                                color: Theme.textPrimary
                                                font.pixelSize: Theme.fontMeta
                                                font.weight: Font.DemiBold
                                                elide: Text.ElideMiddle
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                text: modelData.sourcePath
                                                color: Theme.textSubtle
                                                font.pixelSize: Theme.fontMeta
                                                elide: Text.ElideMiddle
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                visible: modelData.cameraKey.length > 0
                                                text: modelData.cameraKey
                                                color: Theme.textSubtle
                                                font.pixelSize: Theme.fontMeta
                                                elide: Text.ElideRight
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 6

                                                ShadowIconButton {
                                                    source: "qrc:/icons/add-folder.svg"
                                                    variant: ShadowIconButton.Quiet
                                                    toolTipText: qsTr("LOCATE MOVED ORIGINAL")
                                                    accessibleName: toolTipText
                                                    enabled: !library.controller.sourceRelinkBusy
                                                    onClicked: {
                                                        library.pendingRelinkLocationId = modelData.locationId
                                                        relinkFileDialog.open()
                                                    }
                                                }

                                                Label {
                                                    Layout.fillWidth: true
                                                    visible: library.controller.sourceRelinkBusy
                                                        && library.pendingRelinkLocationId === modelData.locationId
                                                    text: qsTr("Verifying complete file…")
                                                    color: Theme.textMuted
                                                    font.pixelSize: Theme.fontMeta
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: library.controller.sourceRelinkStatusText.length > 0
                                        text: library.controller.sourceRelinkStatusText
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.fontMeta
                                        wrapMode: Text.WordWrap
                                    }

                                    ShadowIconButton {
                                        Layout.alignment: Qt.AlignLeft
                                        visible: library.controller.missingSourceLocationsHasMore
                                        source: "qrc:/icons/redo.svg"
                                        variant: ShadowIconButton.Secondary
                                        toolTipText: qsTr("LOAD MORE LOCATIONS")
                                        accessibleName: toolTipText
                                        enabled: !library.controller.missingSourceLocationsBusy
                                        onClicked: library.controller.loadMoreMissingSourceLocations()
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Select a moved original to verify it. Shadow creates no new Library source and changes nothing unless the complete file identity matches exactly.")
                                        color: Theme.textSubtle
                                        font.pixelSize: Theme.fontMeta
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: library.controller.librarySourceHealth.length === 0
                        && !library.controller.librarySourceHealthBusy
                    text: qsTr("No source scans yet. Import a folder to establish one.")
                    color: Theme.textSubtle
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }
            }

            RowLayout {
                Layout.fillWidth: true

                Label {
                    text: qsTr("CATALOG")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.5
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: qsTr("%L1 photos").arg(library.controller.itemCount)
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontMeta
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: sourceContent.implicitHeight + 32
                radius: 8
                color: Theme.panelRaised
                border.color: Theme.border

                RowLayout {
                    id: sourceContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 16
                    anchors.rightMargin: 12
                    spacing: 12

                    Rectangle {
                        Layout.preferredWidth: 36
                        Layout.preferredHeight: 36
                        radius: Theme.controlRadius
                        color: Theme.surfaceSubtle

                        ShadowIcon {
                            anchors.centerIn: parent
                            source: "qrc:/icons/add-folder.svg"
                            color: Theme.textMuted
                            size: 18
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("IMPORT FROM FOLDER")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSection
                            font.weight: Font.DemiBold
                        }

                        Label {
                            Layout.fillWidth: true
                            text: library.controller.folderPath.length > 0
                                ? library.controller.folderPath
                                : qsTr("Choose a folder to add photos")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                            elide: Text.ElideMiddle
                        }
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/add-folder.svg"
                        variant: ShadowIconButton.Secondary
                        toolTipText: library.controller.folderPath.length > 0
                            ? qsTr("CHOOSE ANOTHER FOLDER")
                            : qsTr("ADD PHOTO FOLDER")
                        accessibleName: toolTipText
                        enabled: !library.controller.scanning
                            && !library.controller.refreshing
                            && !library.controller.busy
                            && !library.controller.loadingMore
                            && !library.controller.comparisonBusy
                            && !library.controller.decisionBusy
                        onClicked: library.chooseFolderRequested()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: activityContent.implicitHeight + 28
                visible: library.activityRunning
                    || Number(library.controller.scanProgress.scanId) > 0
                radius: 8
                color: Theme.panel
                border.color: Theme.border

                ColumnLayout {
                    id: activityContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("LIBRARY ACTIVITY")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontMeta
                            font.weight: Font.DemiBold
                        }

                        BusyIndicator {
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            visible: library.activityRunning
                            running: visible
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: library.controller.statusText
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        elide: Text.ElideRight
                    }

                    ProgressBar {
                        Layout.fillWidth: true
                        visible: library.activityRunning
                        indeterminate: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: library.controller.scanning

                        Item { Layout.fillWidth: true }

                        ShadowIconButton {
                            source: "qrc:/icons/clear.svg"
                            variant: ShadowIconButton.Danger
                            toolTipText: qsTr("STOP IMPORT")
                            accessibleName: toolTipText
                            enabled: library.controller.scanProgress.phase
                                !== "cancelling"
                            onClicked: library.controller.cancelScan()
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow keeps catalog decisions, previews and edit history in its local Library while source photos stay untouched.")
                color: Theme.textSubtle
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
                lineHeight: 1.35
            }

            Item { Layout.preferredHeight: 32 }
        }
    }
}
