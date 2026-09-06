pragma ComponentBehavior: Bound
pragma Translator: "Main"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property var editor
    required property var editPreviewPresentation
    required property var opticsProfileLibrary
    required property var lutLibrary
    required property var pipeline

    width: 1480
    height: 920
    minimumWidth: 1200
    minimumHeight: 680
    visible: true
    color: Theme.window
    title: Qt.platform.os === "osx" ? "" : qsTr("Shadow · Precision")

    onClosing: function(close) {
        close.accepted = false
        pipeline.cancel()
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            Label {
                text: window.title.length > 0 ? window.title : qsTr("Shadow · Precision")
                color: Theme.textPrimary
            }

            Item { Layout.fillWidth: true }

            Label {
                Layout.maximumWidth: 420
                elide: Text.ElideRight
                text: pipeline.statusText
                color: Theme.textMuted
            }

            Button {
                text: qsTranslate("ExportDialog", "CANCEL")
                onClicked: pipeline.cancel()
            }

            Button {
                text: qsTranslate("ExportDialog", "SAVE")
                enabled: editor.active && !pipeline.busy
                onClicked: pipeline.complete()
            }
        }
    }

    LutManagerWindow {
        id: lutManager
        lutLibrary: window.lutLibrary
    }

    OpticsProfileManagerWindow {
        id: opticsProfileManager
        editor: window.editor
        opticsProfileLibrary: window.opticsProfileLibrary
    }

    PrecisionWorkspace {
        anchors.fill: parent
        editor: window.editor
        editPreviewPresentation: window.editPreviewPresentation
        lutLibrary: window.lutLibrary
        captureMetadata: ({})
        onOpenLutLibraryRequested: lutManager.openManager()
        onOpenOpticsProfileLibraryRequested: opticsProfileManager.openManager()
        onReturnToReviewRequested: window.pipeline.cancel()
    }
}
