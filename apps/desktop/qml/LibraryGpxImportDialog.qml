pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root

    required property var controller
    property var targets: []
    property url gpxFile: ""
    readonly property var preview: controller.libraryGpxPreview

    title: qsTr("Import GPS Track")
    width: 580
    height: 570
    minimumWidth: 480
    minimumHeight: 500
    color: Theme.window
    flags: Qt.Dialog

    function present(selectedTargets) {
        targets = selectedTargets
        gpxFile = ""
        show()
        raise()
        requestActivate()
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Choose a GPX track")
        nameFilters: [qsTr("GPX tracks (*.gpx)"), qsTr("All files (*)")]
        onAccepted: root.gpxFile = selectedFile
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 16

        Label {
            Layout.fillWidth: true
            text: qsTr("Match selected photos to a GPS track")
            color: Theme.textPrimary
            font.pixelSize: 18
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Shadow matches each effective capture time against UTC track points. Preview the result before applying it.")
            color: Theme.textMuted
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: root.gpxFile.toString().length > 0
                    ? root.gpxFile.toString().replace("file://", "")
                    : qsTr("No GPX track selected")
                color: root.gpxFile.toString().length > 0
                    ? Theme.textPrimary : Theme.textMuted
                elide: Text.ElideMiddle
            }

            ShadowButton {
                text: qsTr("CHOOSE GPX")
                variant: ShadowButton.Ghost
                enabled: !root.controller.libraryMetadataBusy
                onClicked: fileDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5
                Label {
                    text: qsTr("Camera clock offset (minutes)")
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
                SpinBox {
                    id: offsetMinutes
                    Layout.fillWidth: true
                    from: -20 * 60
                    to: 20 * 60
                    value: 0
                    editable: true
                    enabled: !root.controller.libraryMetadataBusy
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5
                Label {
                    text: qsTr("Maximum gap (seconds)")
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
                SpinBox {
                    id: maximumGap
                    Layout.fillWidth: true
                    from: 0
                    to: 86400
                    value: 300
                    editable: true
                    enabled: !root.controller.libraryMetadataBusy
                }
            }
        }

        ShadowButton {
            text: qsTr("PREVIEW MATCHES")
            enabled: !root.controller.libraryMetadataBusy
                && root.gpxFile.toString().length > 0
                && root.targets.length > 0
            onClicked: root.controller.previewLibraryGpxImport(
                root.gpxFile, root.targets,
                offsetMinutes.value * 60, maximumGap.value)
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.preview.previewId ? 150 : 74
            radius: 8
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: root.preview.previewId
                        ? qsTr("%L1 matched · %L2 unmatched").arg(
                            root.preview.matchedPhotoCount).arg(
                            root.preview.unmatchedPhotoCount)
                        : qsTr("%L1 selected photos").arg(root.targets.length)
                    color: Theme.textPrimary
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    visible: Boolean(root.preview.previewId)
                    text: qsTr("Only matched photos will receive a GPS correction. Capture times are not changed.")
                    color: Theme.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.controller.libraryMetadataStatusCode
                        === "failed"
                    text: qsTr("Could not prepare GPS matches: %1").arg(
                        root.controller.libraryMetadataErrorText)
                    color: Theme.errorText
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true

            ShadowButton {
                text: qsTr("CANCEL")
                variant: ShadowButton.Ghost
                enabled: !root.controller.libraryMetadataBusy
                onClicked: root.close()
            }

            Item { Layout.fillWidth: true }

            ShadowButton {
                text: qsTr("APPLY GPS")
                enabled: !root.controller.libraryMetadataBusy
                    && Boolean(root.preview.previewId)
                    && Number(root.preview.matchedPhotoCount) > 0
                onClicked: root.controller.applyLibraryGpxImport(
                    root.preview.previewId)
            }
        }
    }

    Connections {
        target: root.controller
        function onLibraryMetadataChanged() {
            if (root.visible
                    && root.controller.libraryMetadataStatusCode === "applied")
                root.close()
        }
    }
}
