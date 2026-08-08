pragma ComponentBehavior: Bound
pragma Translator: "LibraryWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Owns folder availability presentation, reversible source removal,
// source-scan evidence, missing-location paging, source-level recovery, and
// confirmed removal of originals that remain unavailable everywhere.
ColumnLayout {
    id: sourceHealth

    required property var controller
    property string pendingRelinkLocationId: ""
    property url pendingRelinkFolder: ""
    property string pendingRecoverSourceId: ""
    property url pendingRecoverFolder: ""
    property string pendingRemoveSourceId: ""
    property string pendingRemoveSourcePath: ""
    property string pendingReconcileScanId: ""
    property string pendingReconcileSourcePath: ""
    property int pendingReconcileCount: 0
    readonly property int missingPhotoCount: {
        let total = 0
        const sources = controller.librarySourceHealth
        for (let index = 0; index < sources.length; ++index) {
            total += Math.max(
                Number(sources[index].notSeenLocations || 0),
                Number(sources[index].suspectedMissingLocations || 0))
        }
        return total
    }
    readonly property int foldersNeedingCheck: {
        let total = 0
        const sources = controller.librarySourceHealth
        for (let index = 0; index < sources.length; ++index) {
            if (Boolean(sources[index].quickInventoryNeedsScan))
                ++total
        }
        return total
    }

    spacing: 8

    FolderDialog {
        id: relinkFolderDialog
        title: qsTr("Choose the folder containing the moved original")
        onAccepted: {
            sourceHealth.pendingRelinkFolder = selectedFolder
            relinkConfirmPopup.open()
        }
    }

    FolderDialog {
        id: recoverSourceFolderDialog
        title: qsTr("Choose the folder containing the missing originals")
        onAccepted: {
            sourceHealth.pendingRecoverFolder = selectedFolder
            recoverSourceConfirmPopup.open()
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
                text: qsTr("Search and add this folder")
                color: Theme.textPrimary
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow will search this folder and its subfolders, reconnect the matching original, then add and scan the selected folder.")
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: sourceHealth.pendingRelinkFolder.toString()
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
                    text: qsTr("SEARCH AND ADD FOLDER")
                    variant: ShadowButton.Primary
                    enabled: !Boolean(sourceHealth.controller.sourceRelinkBusy)
                        && !Boolean(sourceHealth.controller.scanning)
                    onClicked: {
                        sourceHealth.controller.relinkMissingSourceLocation(
                            sourceHealth.pendingRelinkLocationId,
                            sourceHealth.pendingRelinkFolder
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

    Popup {
        id: recoverSourceConfirmPopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: 410
        padding: 18
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 10
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.warningBorder
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: qsTr("Locate missing originals")
                color: Theme.textPrimary
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow will verify matching originals, add this folder, and replace the unavailable folder when every original is recovered.")
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: sourceHealth.pendingRecoverFolder.toString()
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
                    onClicked: recoverSourceConfirmPopup.close()
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Primary
                    text: qsTr("LOCATE ORIGINALS")
                    enabled: !sourceHealth.controller.sourceRelinkBusy
                        && !sourceHealth.controller.scanning
                    onClicked: {
                        sourceHealth.controller.recoverLibrarySource(
                            sourceHealth.pendingRecoverSourceId,
                            sourceHealth.pendingRecoverFolder
                        )
                        recoverSourceConfirmPopup.close()
                    }
                }
            }
        }
    }

    Popup {
        id: reconcileConfirmPopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: 410
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
                text: qsTr("Remove missing photos?")
                color: Theme.textPrimary
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow will check every known original path and remove only photos that are still unavailable. Edits and source records are retained for recovery.")
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("%L1 missing photos · %2")
                    .arg(sourceHealth.pendingReconcileCount.toLocaleString())
                    .arg(sourceHealth.pendingReconcileSourcePath)
                color: Theme.warningText
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
                    onClicked: reconcileConfirmPopup.close()
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Danger
                    text: qsTr("REMOVE MISSING PHOTOS")
                    enabled: !sourceHealth.controller.librarySourceReconcileBusy
                        && !sourceHealth.controller.scanning
                    onClicked: {
                        sourceHealth.controller.reconcileMissingSourcePhotos(
                            sourceHealth.pendingReconcileScanId,
                            sourceHealth.pendingReconcileSourcePath
                        )
                        reconcileConfirmPopup.close()
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

        Label {
            visible: sourceHealth.missingPhotoCount > 0
            text: qsTr("%L1 MISSING")
                .arg(sourceHealth.missingPhotoCount.toLocaleString())
            color: Theme.warningText
            font.pixelSize: Theme.fontMeta
            font.weight: Font.DemiBold
        }

        Label {
            visible: sourceHealth.foldersNeedingCheck > 0
            text: qsTr("%L1 FOLDERS NEED CHECK")
                .arg(sourceHealth.foldersNeedingCheck.toLocaleString())
            color: Theme.textSecondary
            font.pixelSize: Theme.fontMeta
            font.weight: Font.DemiBold
        }

        BusyIndicator {
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
            visible: sourceHealth.controller.librarySourceHealthBusy
            running: visible
        }

        ShadowIconButton {
            visible: !sourceHealth.controller.librarySourceHealthBusy
            source: "qrc:/icons/refresh.svg"
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
                readonly property bool folderUnavailable:
                    Boolean(modelData.hasQuickInventory)
                    && !Boolean(modelData.sourceRootAvailable)
                readonly property int missingCount: Math.max(
                    Number(modelData.notSeenLocations || 0),
                    Number(modelData.suspectedMissingLocations || 0))
                readonly property bool hasMissingPhotos:
                    !folderUnavailable && missingCount > 0
                readonly property bool hasConfirmedMissingPhotos:
                    Boolean(modelData.hasLatestCompletedScan)
                    && Number(modelData.notSeenLocations || 0) > 0

                Layout.fillWidth: true
                implicitHeight: sourceHealthContent.implicitHeight + 28
                radius: 8
                color: folderUnavailable
                    ? Theme.dangerSurface
                    : hasMissingPhotos ? Theme.warningSurface : Theme.panelRaised
                border.width: 1
                border.color: folderUnavailable
                    ? Theme.dangerBorder
                    : hasMissingPhotos ? Theme.warningBorder : Theme.border

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
                            color: sourceRow.folderUnavailable
                                ? Theme.dangerText
                                : sourceRow.hasMissingPhotos
                                    ? Theme.warningText
                                    : sourceRow.modelData.enabled
                                        ? Theme.textMuted : Theme.textSubtle
                            size: 16
                        }

                        Rectangle {
                            visible: sourceRow.folderUnavailable
                                || sourceRow.hasMissingPhotos
                            implicitWidth: 18
                            implicitHeight: 18
                            radius: 9
                            color: sourceRow.folderUnavailable
                                ? Theme.dangerText : Theme.warningText

                            Label {
                                anchors.centerIn: parent
                                text: "!"
                                color: Theme.panelRaised
                                font.pixelSize: 12
                                font.weight: Font.Bold
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: sourceRow.modelData.sourcePath
                            color: sourceRow.folderUnavailable
                                ? Theme.dangerText
                                : sourceRow.hasMissingPhotos
                                    ? Theme.warningText
                                    : sourceRow.modelData.enabled
                                        ? Theme.textPrimary : Theme.textMuted
                            font.pixelSize: Theme.fontSection
                            font.weight: Font.DemiBold
                            font.strikeout: sourceRow.folderUnavailable
                            elide: Text.ElideMiddle
                        }

                        Label {
                            text: sourceRow.folderUnavailable
                                ? qsTr("FOLDER UNAVAILABLE")
                                : sourceRow.hasMissingPhotos
                                    ? qsTr("%L1 MISSING")
                                        .arg(sourceRow.missingCount.toLocaleString())
                                    : sourceRow.modelData.enabled
                                        ? qsTr("ACTIVE") : qsTr("PAUSED")
                            color: sourceRow.folderUnavailable
                                ? Theme.dangerText
                                : sourceRow.hasMissingPhotos
                                    ? Theme.warningText
                                    : sourceRow.modelData.enabled
                                        ? Theme.accent : Theme.textSubtle
                            font.pixelSize: Theme.fontMeta
                            font.weight: Font.DemiBold
                        }

                        ShadowIconButton {
                            visible: !sourceRow.folderUnavailable
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
                        text: sourceRow.folderUnavailable
                            ? qsTr("Folder unavailable · %L1 catalog photos need recovery.")
                                .arg(Number(sourceRow.modelData.knownLocations).toLocaleString())
                            : sourceRow.modelData.quickInventoryNeedsScan
                                ? qsTr("Quick check found %L1 supported files; the Catalog expects %L2. Run a full check.")
                                    .arg(Number(sourceRow.modelData.currentSupportedFiles).toLocaleString())
                                    .arg(Number(sourceRow.modelData.knownLocations).toLocaleString())
                            : !sourceRow.modelData.hasLatestCompletedScan
                            ? qsTr("No completed scan has been recorded yet.")
                            : Number(sourceRow.modelData.notSeenLocations) === 0
                                ? qsTr("The latest scan accounted for all known locations.")
                                : qsTr("%L1 locations were not seen in this scan.")
                                    .arg(Number(sourceRow.modelData.notSeenLocations).toLocaleString())
                        color: sourceRow.folderUnavailable
                            ? Theme.dangerText
                            : sourceRow.hasMissingPhotos
                                ? Theme.warningText : Theme.textMuted
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

                        Label {
                            visible: sourceRow.modelData.hasQuickInventory
                                && sourceRow.modelData.sourceRootAvailable
                            text: qsTr("NOW  %L1")
                                .arg(Number(sourceRow.modelData.currentSupportedFiles).toLocaleString())
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                        }

                        Item { Layout.fillWidth: true }

                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: Boolean(sourceRow.modelData.quickInventoryNeedsScan)
                            && !sourceRow.folderUnavailable
                            && !sourceRow.hasMissingPhotos

                        Item { Layout.fillWidth: true }

                        ShadowButton {
                            compact: true
                            variant: ShadowButton.Primary
                            text: qsTr("CHECK FOLDER")
                            enabled: !sourceHealth.controller.scanning
                                && !sourceHealth.controller.refreshing
                            onClicked: sourceHealth.controller.verifyLibrarySource(
                                sourceRow.modelData.sourcePath)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: sourceRow.folderUnavailable
                            || sourceRow.hasMissingPhotos
                        spacing: 8

                        ShadowButton {
                            compact: true
                            variant: ShadowButton.Secondary
                            text: sourceRow.folderUnavailable
                                ? qsTr("RELOCATE FOLDER")
                                : qsTr("LOCATE MISSING PHOTOS")
                            enabled: !sourceHealth.controller.sourceRelinkBusy
                                && !sourceHealth.controller.scanning
                            onClicked: {
                                sourceHealth.pendingRecoverSourceId =
                                    sourceRow.modelData.sourceId
                                recoverSourceFolderDialog.open()
                            }
                        }

                        Item { Layout.fillWidth: true }

                        ShadowButton {
                            visible: sourceRow.folderUnavailable
                            compact: true
                            variant: ShadowButton.Danger
                            text: qsTr("REMOVE FROM LIBRARY")
                            enabled: !sourceHealth.controller.librarySourceRemovalBusy
                                && !sourceHealth.controller.scanning
                            onClicked: {
                                sourceHealth.pendingRemoveSourceId =
                                    sourceRow.modelData.sourceId
                                sourceHealth.pendingRemoveSourcePath =
                                    sourceRow.modelData.sourcePath
                                removeSourceConfirmPopup.open()
                            }
                        }

                        ShadowButton {
                            visible: sourceRow.hasMissingPhotos
                                && !sourceRow.hasConfirmedMissingPhotos
                            compact: true
                            variant: ShadowButton.Primary
                            text: qsTr("CHECK FOLDER")
                            enabled: !sourceHealth.controller.scanning
                                && !sourceHealth.controller.refreshing
                            onClicked: sourceHealth.controller.verifyLibrarySource(
                                sourceRow.modelData.sourcePath)
                        }

                        ShadowButton {
                            visible: sourceRow.hasConfirmedMissingPhotos
                            compact: true
                            variant: ShadowButton.Danger
                            text: qsTr("REMOVE MISSING PHOTOS")
                            enabled: !sourceHealth.controller.librarySourceReconcileBusy
                                && !sourceHealth.controller.scanning
                            onClicked: {
                                sourceHealth.pendingReconcileScanId =
                                    sourceRow.modelData.scanSessionId
                                sourceHealth.pendingReconcileSourcePath =
                                    sourceRow.modelData.sourcePath
                                sourceHealth.pendingReconcileCount = Number(
                                    sourceRow.modelData.notSeenLocations)
                                reconcileConfirmPopup.open()
                            }
                        }

                        ShadowIconButton {
                            visible: sourceRow.hasConfirmedMissingPhotos
                                && sourceHealth.controller.missingSourceLocationScanId
                                    !== sourceRow.modelData.scanSessionId
                            source: "qrc:/icons/source-missing.svg"
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
                                            toolTipText: qsTr("SEARCH FOLDER FOR ORIGINAL")
                                            accessibleName: toolTipText
                                            enabled: !sourceHealth.controller.sourceRelinkBusy
                                            onClicked: {
                                                sourceHealth.pendingRelinkLocationId =
                                                    missingLocationRow.modelData.locationId
                                                relinkFolderDialog.open()
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
                                source: "qrc:/icons/chevron-down.svg"
                                variant: ShadowIconButton.Secondary
                                toolTipText: qsTr("LOAD MORE LOCATIONS")
                                accessibleName: toolTipText
                                enabled: !sourceHealth.controller.missingSourceLocationsBusy
                                onClicked: sourceHealth.controller.loadMoreMissingSourceLocations()
                            }

                            Label {
                                Layout.fillWidth: true
                text: qsTr("Shadow will search this folder and its subfolders, reconnect the matching original, then add and scan the selected folder.")
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
