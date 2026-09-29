pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root

    required property var controller
    property string photoId: ""
    property string validationError: ""
    readonly property var metadata: controller.libraryMetadata

    title: qsTr("Edit Time and Location")
    width: 520
    height: 560
    minimumWidth: 440
    minimumHeight: 500
    color: Theme.window
    flags: Qt.Dialog

    function formatDateTime(unixSeconds) {
        if (!Number.isFinite(Number(unixSeconds)))
            return ""
        const date = new Date(Number(unixSeconds) * 1000)
        function pad(value) { return String(value).padStart(2, "0") }
        return date.getFullYear() + "-" + pad(date.getMonth() + 1)
            + "-" + pad(date.getDate()) + " " + pad(date.getHours())
            + ":" + pad(date.getMinutes()) + ":" + pad(date.getSeconds())
    }

    function syncFields() {
        const value = metadata || ({})
        if (String(value.photoId || "") !== photoId)
            return
        validationError = ""
        captureTimeField.text = value.hasEffectiveCaptureTime
            ? formatDateTime(value.effectiveCapturedAtUnixSeconds) : ""
        latitudeField.text = value.hasEffectiveCoordinates
            ? Number(value.effectiveLatitude).toLocaleString(
                Qt.locale("C"), "f", 7) : ""
        longitudeField.text = value.hasEffectiveCoordinates
            ? Number(value.effectiveLongitude).toLocaleString(
                Qt.locale("C"), "f", 7) : ""
        placeField.text = String(value.effectivePlaceName || "")
    }

    function present(selectedPhotoId) {
        photoId = selectedPhotoId
        controller.requestLibraryMetadata(photoId)
        show()
        raise()
        requestActivate()
    }

    onMetadataChanged: syncFields()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 16

        Label {
            Layout.fillWidth: true
            text: qsTr("Non-destructive metadata corrections")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontHeading
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("The camera EXIF remains unchanged. Rescanning the photo will not overwrite these corrections.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSection
            wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        Label {
            text: qsTr("Capture time")
            color: Theme.textSecondary
            font.weight: Font.DemiBold
        }

        ShadowTextField {
            id: captureTimeField
            Layout.fillWidth: true
            placeholderText: qsTr("YYYY-MM-DD HH:MM:SS (local time)")
            enabled: !root.controller.libraryMetadataBusy
        }

        RowLayout {
            Layout.fillWidth: true

            ShadowButton {
                text: qsTr("SAVE TIME")
                enabled: !root.controller.libraryMetadataBusy
                    && captureTimeField.text.trim().length > 0
                onClicked: {
                    const localDate = new Date(
                        captureTimeField.text.trim().replace(" ", "T"))
                    if (isNaN(localDate.getTime())) {
                        root.validationError =
                            qsTr("Enter a valid local date and time.")
                    } else {
                        root.validationError = ""
                        root.controller.setLibraryCaptureTime(
                            root.photoId, "set",
                            Math.floor(localDate.getTime() / 1000))
                    }
                }
            }

            ShadowButton {
                text: qsTr("USE CAMERA VALUE")
                variant: ShadowButton.Ghost
                enabled: !root.controller.libraryMetadataBusy
                onClicked: root.controller.setLibraryCaptureTime(
                    root.photoId, "inherit", 0)
            }

            ShadowButton {
                text: qsTr("CLEAR")
                variant: ShadowButton.Ghost
                enabled: !root.controller.libraryMetadataBusy
                onClicked: root.controller.setLibraryCaptureTime(
                    root.photoId, "clear", 0)
            }
        }

        Label {
            Layout.topMargin: 4
            text: qsTr("GPS coordinates")
            color: Theme.textSecondary
            font.weight: Font.DemiBold
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            ShadowTextField {
                id: latitudeField
                Layout.fillWidth: true
                placeholderText: qsTr("Latitude")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                enabled: !root.controller.libraryMetadataBusy
            }

            ShadowTextField {
                id: longitudeField
                Layout.fillWidth: true
                placeholderText: qsTr("Longitude")
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                enabled: !root.controller.libraryMetadataBusy
            }
        }

        ShadowTextField {
            id: placeField
            Layout.fillWidth: true
            placeholderText: qsTr("Place name (optional)")
            enabled: !root.controller.libraryMetadataBusy
        }

        RowLayout {
            Layout.fillWidth: true

            ShadowButton {
                text: qsTr("SAVE LOCATION")
                enabled: !root.controller.libraryMetadataBusy
                    && latitudeField.text.trim().length > 0
                    && longitudeField.text.trim().length > 0
                onClicked: root.controller.setLibraryCoordinates(
                    root.photoId, "set",
                    Number.fromLocaleString(
                        Qt.locale("C"), latitudeField.text.trim()),
                    Number.fromLocaleString(
                        Qt.locale("C"), longitudeField.text.trim()),
                    placeField.text.trim())
            }

            ShadowButton {
                text: qsTr("USE CAMERA VALUE")
                variant: ShadowButton.Ghost
                enabled: !root.controller.libraryMetadataBusy
                onClicked: root.controller.setLibraryCoordinates(
                    root.photoId, "inherit", 0, 0, "")
            }

            ShadowButton {
                text: qsTr("CLEAR")
                variant: ShadowButton.Ghost
                enabled: !root.controller.libraryMetadataBusy
                onClicked: root.controller.setLibraryCoordinates(
                    root.photoId, "clear", 0, 0, "")
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.validationError.length > 0
                || root.controller.libraryMetadataStatusCode === "failed"
                || root.controller.libraryMetadataStatusCode === "saved"
            text: root.validationError.length > 0
                ? root.validationError
                : root.controller.libraryMetadataStatusCode === "failed"
                ? qsTr("Could not save metadata correction: %1").arg(
                    root.controller.libraryMetadataErrorText)
                : qsTr("Metadata correction saved")
            color: root.validationError.length > 0
                || root.controller.libraryMetadataStatusCode === "failed"
                ? Theme.errorText : Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        Item { Layout.fillHeight: true }

        ShadowButton {
            Layout.alignment: Qt.AlignRight
            text: qsTr("DONE")
            variant: ShadowButton.Ghost
            onClicked: root.close()
        }
    }
}
