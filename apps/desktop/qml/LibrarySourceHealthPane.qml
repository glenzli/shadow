pragma ComponentBehavior: Bound
pragma Translator: "LibraryWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Owns reversible source removal, source-scan evidence, missing-location
// paging, and the exact-content relink confirmation transaction. It never
// creates a new Library source.
ColumnLayout {
    id: sourceHealth

    required property var controller
    property string pendingRelinkLocationId: ""
    property url pendingRelinkCandidate: ""
    property string pendingRemoveSourceId: ""
    property string pendingRemoveSourcePath: ""

    spacing: 8

    FileDialog {
        id: relinkFileDialog
        title: qsTr("Choose the moved original file")
        fileMode: FileDialog.OpenFile
        onAccepted: {
            sourceHealth.pendingRelinkCandidate = selectedFile
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
                text: sourceHealth.pendingRelinkCandidate.toString()
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
                        sourceHealth.controller.relinkMissingSourceLocation(
                            sourceHealth.pendingRelinkLocationId,
                            sourceHealth.pendingRelinkCandidate
                        )
                        relinkConfirmPopup.close()
                    }
                }
            }
        }
    }

    Popup {
        id: removeSourceConfirmPopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: 390
        padding: 18
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: 10
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.dangerBorder
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: qsTr("Remove Library folder?")
                color: Theme.textPrimary
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Remove this folder from Shadow’s Library? Photos available only through this folder leave the Gallery. Edits and original files are kept, and return if the folder is added again.")
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: sourceHealth.pendingRemoveSourcePath
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideMiddle
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item { Layout.fillWidth: true }

                ShadowButton {
                    compact: true
                    text: qsTr("CANCEL")
                    onClicked: removeSourceConfirmPopup.close()
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Danger
                    text: qsTr("REMOVE FOLDER")
                    enabled: !sourceHealth.controller.librarySourceRemovalBusy
                        && !sourceHealth.controller.scanning
                    onClicked: {
                        sourceHealth.controller.removeLibrarySource(
                            sourceHealth.pendingRemoveSourceId,
                            sourceHealth.pendingRemoveSourcePath
                        )
                        removeSourceConfirmPopup.close()
                    }
                }
            }
        }
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
            visible: sourceHealth.controller.librarySourceHealthBusy
            running: visible
        }

        ShadowIconButton {
            visible: !sourceHealth.controller.librarySourceHealthBusy
            source: "qrc:/icons/history.svg"
            variant: ShadowIconButton.Quiet
            toolTipText: qsTr("REFRESH SOURCE HEALTH")
            accessibleName: toolTipText
            onClicked: sourceHealth.controller.refreshLibrarySourceHealth()
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 8

        Repeater {
            model: sourceHealth.controller.librarySourceHealth

            delegate: Rectangle {
                id: sourceRow
                required property var modelData

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
                            color: sourceRow.modelData.enabled
                                ? Theme.textMuted : Theme.textSubtle
                            size: 16
                        }

                        Label {
                            Layout.fillWidth: true
                            text: sourceRow.modelData.sourcePath
                            color: sourceRow.modelData.enabled
                                ? Theme.textPrimary : Theme.textMuted
                            font.pixelSize: Theme.fontSection
                            font.weight: Font.DemiBold
                            elide: Text.ElideMiddle
                        }

                        Label {
                            text: sourceRow.modelData.enabled
                                ? qsTr("ACTIVE") : qsTr("PAUSED")
                            color: sourceRow.modelData.enabled
                                ? Theme.accent : Theme.textSubtle
                            font.pixelSize: Theme.fontMeta
                            font.weight: Font.DemiBold
                        }

                        ShadowIconButton {
                            source: "qrc:/icons/trash.svg"
                            variant: ShadowIconButton.Quiet
                            enabled: !sourceHealth.controller.librarySourceRemovalBusy
                                && !sourceHealth.controller.scanning
                            toolTipText: qsTr("REMOVE FOLDER FROM LIBRARY")
                            accessibleName: toolTipText
                            onClicked: {
                                sourceHealth.pendingRemoveSourceId =
                                    sourceRow.modelData.sourceId
                                sourceHealth.pendingRemoveSourcePath =
                                    sourceRow.modelData.sourcePath
                                removeSourceConfirmPopup.open()
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: !sourceRow.modelData.hasLatestCompletedScan
                            ? qsTr("No completed scan has been recorded yet.")
                            : Number(sourceRow.modelData.notSeenLocations) === 0
                                ? qsTr("The latest scan accounted for all known locations.")
                                : qsTr("%L1 locations were not seen in this scan.")
                                    .arg(Number(sourceRow.modelData.notSeenLocations).toLocaleString())
                        color: Number(sourceRow.modelData.notSeenLocations) > 0
                            ? Theme.textSecondary : Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: sourceRow.modelData.hasLatestCompletedScan
                        spacing: 18

                        Label {
                            text: qsTr("KNOWN  %L1")
                                .arg(Number(sourceRow.modelData.knownLocations).toLocaleString())
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                        }

                        Label {
                            text: qsTr("SEEN  %L1")
                                .arg(Number(sourceRow.modelData.seenLocations).toLocaleString())
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: sourceRow.modelData.hasLatestCompletedScan
                            && Number(sourceRow.modelData.notSeenLocations) > 0
                        text: qsTr("This is scan evidence for this source only; photos may remain available elsewhere.")
                        color: Theme.textSubtle
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: sourceRow.modelData.hasLatestCompletedScan
                            && Number(sourceRow.modelData.notSeenLocations) > 0

                        Item { Layout.fillWidth: true }

                        ShadowIconButton {
                            visible: sourceHealth.controller.missingSourceLocationScanId
                                !== sourceRow.modelData.scanSessionId
                            source: "qrc:/icons/metadata.svg"
                            variant: ShadowIconButton.Quiet
                            toolTipText: qsTr("REVIEW NOT-SEEN LOCATIONS")
                            accessibleName: toolTipText
                            onClicked: sourceHealth.controller.openMissingSourceLocationReview(
                                sourceRow.modelData.scanSessionId
                            )
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        visible: sourceHealth.controller.missingSourceLocationScanId
                            === sourceRow.modelData.scanSessionId
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
                                    visible: sourceHealth.controller.missingSourceLocationsBusy
                                    running: visible
                                }

                                ShadowIconButton {
                                    source: "qrc:/icons/clear.svg"
                                    variant: ShadowIconButton.Quiet
                                    toolTipText: qsTr("CLOSE LOCATION REVIEW")
                                    accessibleName: toolTipText
                                    onClicked: sourceHealth.controller.closeMissingSourceLocationReview()
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: sourceHealth.controller.missingSourceLocations.length === 0
                                    && !sourceHealth.controller.missingSourceLocationsBusy
                                text: qsTr("No locations are available for this completed scan.")
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontMeta
                                wrapMode: Text.WordWrap
                            }

                            Repeater {
                                model: sourceHealth.controller.missingSourceLocations

                                delegate: ColumnLayout {
                                    id: missingLocationRow
                                    required property var modelData

                                    Layout.fillWidth: true
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: missingLocationRow.modelData.title
                                        color: Theme.textPrimary
                                        font.pixelSize: Theme.fontMeta
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideMiddle
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: missingLocationRow.modelData.sourcePath
                                        color: Theme.textSubtle
                                        font.pixelSize: Theme.fontMeta
                                        elide: Text.ElideMiddle
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: missingLocationRow.modelData.cameraKey.length > 0
                                        text: missingLocationRow.modelData.cameraKey
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
                                            enabled: !sourceHealth.controller.sourceRelinkBusy
                                            onClicked: {
                                                sourceHealth.pendingRelinkLocationId =
                                                    missingLocationRow.modelData.locationId
                                                relinkFileDialog.open()
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            visible: sourceHealth.controller.sourceRelinkBusy
                                                && sourceHealth.pendingRelinkLocationId
                                                    === missingLocationRow.modelData.locationId
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
                                visible: sourceHealth.controller.sourceRelinkStatusText.length > 0
                                text: sourceHealth.controller.sourceRelinkStatusText
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontMeta
                                wrapMode: Text.WordWrap
                            }

                            ShadowIconButton {
                                Layout.alignment: Qt.AlignLeft
                                visible: sourceHealth.controller.missingSourceLocationsHasMore
                                source: "qrc:/icons/redo.svg"
                                variant: ShadowIconButton.Secondary
                                toolTipText: qsTr("LOAD MORE LOCATIONS")
                                accessibleName: toolTipText
                                enabled: !sourceHealth.controller.missingSourceLocationsBusy
                                onClicked: sourceHealth.controller.loadMoreMissingSourceLocations()
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
            visible: sourceHealth.controller.librarySourceHealth.length === 0
                && !sourceHealth.controller.librarySourceHealthBusy
            text: qsTr("No source scans yet. Import a folder to establish one.")
            color: Theme.textSubtle
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }
    }

}
