pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root

    required property var controller
    property var targets: []
    property bool previewRequested: false
    property bool applying: false
    readonly property var preview: controller.libraryCaptureTimePreview
    readonly property bool previewReady: previewRequested
        && !controller.libraryMetadataBusy
        && controller.libraryMetadataStatusCode === "ready"
        && Boolean(preview.previewId)

    title: qsTr("Adjust Capture Times")
    width: 560
    height: 540
    minimumWidth: 460
    minimumHeight: 500
    color: Theme.window
    flags: Qt.Dialog

    function present(selectedTargets) {
        targets = selectedTargets
        previewRequested = false
        applying = false
        offsetMinutes.value = 0
        show()
        raise()
        requestActivate()
    }

    function offsetLabel(minutes) {
        const sign = minutes >= 0 ? "+" : "−"
        const absolute = Math.abs(minutes)
        const hours = Math.floor(absolute / 60)
        const remaining = absolute % 60
        return sign + String(hours).padStart(2, "0") + ":"
            + String(remaining).padStart(2, "0")
    }

    function formatDateTime(unixSeconds, present) {
        if (!present)
            return qsTr("No capture time")
        const date = new Date(Number(unixSeconds) * 1000)
        function pad(value) { return String(value).padStart(2, "0") }
        return date.getFullYear() + "-" + pad(date.getMonth() + 1)
            + "-" + pad(date.getDate()) + " " + pad(date.getHours())
            + ":" + pad(date.getMinutes()) + ":" + pad(date.getSeconds())
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 16

        Label {
            Layout.fillWidth: true
            text: qsTr("Correct capture times for selected photos")
            color: Theme.textPrimary
            font.pixelSize: 18
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Preview a relative clock correction before applying it. The camera EXIF remains unchanged.")
            color: Theme.textMuted
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5

                Label {
                    text: qsTr("Clock offset (minutes)")
                    color: Theme.textMuted
                    font.pixelSize: 10
                }

                SpinBox {
                    id: offsetMinutes
                    Layout.fillWidth: true
                    from: -840
                    to: 840
                    stepSize: 15
                    editable: true
                    enabled: !root.controller.libraryMetadataBusy
                    onValueModified: root.previewRequested = false
                }
            }

            Label {
                Layout.alignment: Qt.AlignBottom
                Layout.bottomMargin: 8
                text: root.offsetLabel(offsetMinutes.value)
                color: Theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            ShadowButton {
                text: qsTr("PREVIEW SHIFT")
                enabled: !root.controller.libraryMetadataBusy
                    && root.targets.length > 0
                    && offsetMinutes.value !== 0
                onClicked: {
                    root.applying = false
                    root.previewRequested = true
                    root.controller.previewLibraryCaptureTimeBatch(
                        root.targets, "shift", offsetMinutes.value * 60)
                }
            }

            ShadowButton {
                text: qsTr("PREVIEW RESTORE CAMERA TIMES")
                variant: ShadowButton.Ghost
                enabled: !root.controller.libraryMetadataBusy
                    && root.targets.length > 0
                onClicked: {
                    root.applying = false
                    root.previewRequested = true
                    root.controller.previewLibraryCaptureTimeBatch(
                        root.targets, "inherit", 0)
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.previewReady ? 160 : 86
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
                    text: root.previewReady
                        ? qsTr("%L1 will change · %L2 skipped").arg(
                            root.preview.applicablePhotoCount).arg(
                            root.preview.skippedPhotoCount)
                        : qsTr("%L1 selected photos").arg(
                            root.targets.length)
                    color: Theme.textPrimary
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.previewReady
                    text: root.preview.mode === "inherit"
                        ? qsTr("Existing capture-time corrections will return to their current camera values.")
                        : qsTr("Every applicable photo receives the same relative clock correction.")
                    color: Theme.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }

                Label {
                    id: exampleLabel

                    Layout.fillWidth: true
                    visible: root.previewReady
                        && root.preview.proposalSample.length > 0
                    readonly property var example: visible
                        ? root.preview.proposalSample[0] : ({})
                    text: qsTr("Example: %1 → %2").arg(
                        root.formatDateTime(
                            exampleLabel.example.beforeCapturedAtUnixSeconds,
                            exampleLabel.example.hasBeforeCaptureTime)).arg(
                        root.formatDateTime(
                            exampleLabel.example.afterCapturedAtUnixSeconds,
                            exampleLabel.example.hasAfterCaptureTime))
                    color: Theme.textSecondary
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.previewRequested
                        && root.controller.libraryMetadataStatusCode
                            === "failed"
                    text: qsTr("Could not prepare capture-time changes: %1").arg(
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
                text: root.preview.mode === "inherit"
                    ? qsTr("RESTORE CAMERA TIMES")
                    : qsTr("APPLY TIME SHIFT")
                enabled: root.previewReady
                    && Number(root.preview.applicablePhotoCount) > 0
                onClicked: {
                    root.applying = true
                    root.controller.applyLibraryCaptureTimeBatch(
                        root.preview.previewId)
                }
            }
        }
    }

    Connections {
        target: root.controller
        function onLibraryMetadataChanged() {
            if (root.visible && root.applying
                    && root.controller.libraryMetadataStatusCode === "applied")
                root.close()
        }
    }
}
