pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window
    required property var preferences
    required property var editor
    required property var interchangeController
    required property var editPreviewPresentation
    required property var opticsProfileLibrary
    required property var lutLibrary
    required property var pipeline

    width: 1480
    height: 920
    minimumWidth: 1100
    minimumHeight: 680
    flags: Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    visible: true
    color: Theme.window
    readonly property string descriptiveTitle: qsTr("Shadow · Independent Editor")
    title: Qt.platform.os === "osx" ? "" : descriptiveTitle

    Binding { target: Theme; property: "effectiveDark"; value: window.preferences.dark }

    function requestClose() {
        if (pipeline.finished || pipeline.photoCount === 0)
            pipeline.cancel()
        else
            closeDialog.open()
    }
    function exportPhotos() {
        if (!pipeline.interactive || pipeline.completedCount > 0)
            pipeline.complete()
        else
            exportDialog.open()
    }
    onClosing: function(close) {
        close.accepted = false
        requestClose()
    }

    Shortcut {
        sequences: [StandardKey.Open]
        enabled: pipeline.interactive && !pipeline.busy && !pipeline.finished
            && pipeline.completedCount === 0
        onActivated: photoPicker.open()
    }
    Shortcut {
        sequence: "Ctrl+Shift+E"
        enabled: pipeline.photoCount > 0 && !pipeline.busy && !pipeline.finished
        onActivated: window.exportPhotos()
    }
    DropArea {
        anchors.fill: parent
        enabled: pipeline.interactive && !pipeline.busy && !pipeline.finished
            && pipeline.completedCount === 0
        onDropped: drop => {
            if (drop.hasUrls) {
                pipeline.addFiles(drop.urls)
                drop.acceptProposedAction()
            }
        }
    }

    header: Column {
        ShadowTitleBar {
            width: parent.width
            hostWindow: window
            Accessible.name: window.descriptiveTitle
            RowLayout {
                anchors.fill: parent
                spacing: 12
                Label {
                    text: "SHADOW"
                    font.pixelSize: Theme.fontSubheading
                    font.weight: Font.DemiBold
                    font.letterSpacing: 2.5
                    color: Theme.textPrimary
                }
                Rectangle {
                    implicitWidth: 1
                    implicitHeight: 18
                    color: Theme.border
                }
                Label {
                    text: qsTr("Independent Editor")
                    font.pixelSize: Theme.fontSubheading
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: qsTr("This session is separate from your Library.")
                    font.pixelSize: Theme.fontMeta
                    color: Theme.textMuted
                }
                ShadowButton {
                    compact: true
                    text: qsTr("Open photos…")
                    visible: pipeline.interactive && !pipeline.finished
                    enabled: !pipeline.busy && pipeline.completedCount === 0
                    onClicked: photoPicker.open()
                }
                ShadowButton {
                    compact: true
                    text: pipeline.finished ? qsTr("Close") : qsTr("Cancel session")
                    onClicked: window.requestClose()
                }
                ShadowButton {
                    compact: true
                    variant: ShadowButton.Primary
                    visible: !pipeline.finished
                    text: pipeline.completedCount > 0
                        ? qsTr("Retry remaining") : qsTr("Export %1 photos…").arg(pipeline.photoCount)
                    enabled: pipeline.photoCount > 0 && !pipeline.busy
                    onClicked: window.exportPhotos()
                }
            }
        }
        ToolBar {
            width: parent.width
            implicitHeight: 44
            leftPadding: 16
            rightPadding: 16
            visible: pipeline.photoCount > 0 && !pipeline.exportPending && !pipeline.finished
            background: Rectangle { color: Theme.chrome }
            contentItem: RowLayout {
                spacing: 8
                ShadowButton {
                    compact: true
                    text: "‹"
                    accessibleName: qsTr("Previous photo")
                    enabled: pipeline.currentIndex > 0 && pipeline.navigationEnabled
                    onClicked: pipeline.selectPhoto(pipeline.currentIndex - 1)
                }
                ShadowComboBox {
                    id: photoSelector
                    objectName: "pipelinePhotoSelector"
                    Layout.preferredWidth: 320
                    model: pipeline.photos
                    textRole: "name"
                    currentIndex: pipeline.currentIndex
                    enabled: pipeline.navigationEnabled
                    onActivated: index => pipeline.selectPhoto(index)
                }
                ShadowButton {
                    compact: true
                    text: "›"
                    accessibleName: qsTr("Next photo")
                    enabled: pipeline.currentIndex + 1 < pipeline.photoCount
                        && pipeline.navigationEnabled
                    onClicked: pipeline.selectPhoto(pipeline.currentIndex + 1)
                }
                Label {
                    text: pipeline.photoCount > 0
                        ? qsTr("%1 / %2").arg(pipeline.currentIndex + 1).arg(pipeline.photoCount) : ""
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: editor.sourcePath
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: errorRow.implicitHeight + 20
            visible: pipeline.errorText.length > 0 || editor.autosaveFailed
            color: Theme.panelRaised
            RowLayout {
                id: errorRow
                anchors.fill: parent
                anchors.margins: 10
                Label {
                    Layout.fillWidth: true
                    text: pipeline.errorText.length > 0 ? pipeline.errorText : editor.autosaveErrorText
                    wrapMode: Text.Wrap
                    color: Theme.errorText
                    font.pixelSize: Theme.fontBody
                }
                ShadowButton {
                    text: qsTr("Retry saving")
                    visible: editor.autosaveFailed
                    enabled: !editor.stateBusy
                    onClicked: editor.retryAutosave()
                }
            }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            PrecisionWorkspace {
                id: workspace
                objectName: "pipelinePrecisionWorkspace"
                anchors.fill: parent
                visible: pipeline.photoCount > 0 && !pipeline.exportPending && !pipeline.finished
                enabled: pipeline.currentPhotoEditable
                editor: window.editor
                interchangeController: window.interchangeController
                editPreviewPresentation: window.editPreviewPresentation
                lutLibrary: window.lutLibrary
                captureMetadata: pipeline.captureMetadata
                onOpenLutLibraryRequested: lutManager.openManager()
                onOpenOpticsProfileLibraryRequested: opticsProfileManager.openManager()
                onReturnToReviewRequested: window.requestClose()
            }
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(560, parent.width - 48)
                visible: pipeline.photoCount === 0 || pipeline.exportPending || pipeline.finished
                spacing: 16
                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    running: pipeline.exportPending
                    visible: running
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: pipeline.finished ? qsTr("Export complete")
                        : pipeline.exportPending ? pipeline.statusText
                        : qsTr("Open one photo or a set of photos")
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontHeading
                    color: Theme.textPrimary
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: pipeline.finished || pipeline.exportPending ? pipeline.outputFolder
                        : qsTr("Drop photos here or choose files. Originals stay unchanged; adjustments are kept for this session.")
                    font.pixelSize: Theme.fontBody
                    color: Theme.textSecondary
                }
                ShadowButton {
                    Layout.alignment: Qt.AlignHCenter
                    visible: !pipeline.finished && !pipeline.exportPending
                    text: qsTr("Open photos…")
                    enabled: pipeline.interactive && !pipeline.busy
                    onClicked: photoPicker.open()
                }
            }
        }
    }
    footer: ToolBar {
        implicitHeight: 40
        background: Rectangle { color: Theme.chrome }
        RowLayout {
            anchors.fill: parent
            anchors.margins: 6
            BusyIndicator { running: pipeline.busy; visible: running; implicitWidth: 28; implicitHeight: 28 }
            Label {
                Layout.fillWidth: true
                text: pipeline.statusText
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideRight
            }
            ShadowButton {
                text: qsTr("Stop export")
                visible: pipeline.exporting
                onClicked: pipeline.stopExport()
            }
        }
    }
    FileDialog {
        id: photoPicker
        title: qsTr("Open photos for independent editing")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Photos (*.nef *.nrw *.cr2 *.cr3 *.arw *.raf *.orf *.rw2 *.pef *.srw *.dng *.jpg *.jpeg *.tif *.tiff *.heic *.heif)"), qsTr("All files (*)")]
        onAccepted: pipeline.addFiles(selectedFiles)
    }
    PipelineExportDialog {
        id: exportDialog
        pipeline: window.pipeline
    }
    Dialog {
        id: closeDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 450
        modal: true
        title: qsTr("Close this editing session?")
        contentItem: Label {
            text: qsTr("Unexported adjustments will be discarded. Originals and files already exported will be kept.")
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontBody
            color: Theme.textPrimary
        }
        footer: DialogButtonBox {
            ShadowButton { text: qsTr("Keep editing"); onClicked: closeDialog.close() }
            ShadowButton { text: qsTr("Close session"); onClicked: { closeDialog.close(); pipeline.cancel() } }
        }
    }
    LutManagerWindow { id: lutManager; lutLibrary: window.lutLibrary }
    OpticsProfileManagerWindow {
        id: opticsProfileManager
        editor: window.editor
        opticsProfileLibrary: window.opticsProfileLibrary
    }
}
