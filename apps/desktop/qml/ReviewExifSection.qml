pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the selected photo's configurable EXIF projection and retry action.
ColumnLayout {
    id: exifSection

    required property var review
    required property var metadataPresentation

    signal openMetadataRequested()

    visible: exifSection.review.selectedPhotoId.length > 0
    spacing: 6

    RowLayout {
        Layout.fillWidth: true

        Label {
            text: qsTranslate("ReviewWorkspace", "EXIF")
            color: exifSection.review.textMuted
            font.pixelSize: 10
            font.weight: Font.DemiBold
            font.letterSpacing: 1.2
        }

        Item { Layout.fillWidth: true }

        ShadowIconButton {
            source: "qrc:/icons/metadata.svg"
            iconSize: 15
            toolTipText: qsTranslate(
                "ReviewWorkspace", "View all photo metadata")
            accessibleName: toolTipText
            enabled: exifSection.review.selectedPhotoId.length > 0
            onClicked: exifSection.openMetadataRequested()
        }
    }

    Label {
        Layout.fillWidth: true
        visible: !exifSection.review.selectedHasMetadata
        text: exifSection.review.controller.photoInspectionFailed
            ? qsTranslate(
                "ReviewWorkspace",
                "Could not load metadata for this photo")
            : exifSection.review.controller.photoInspectionBusy
                ? qsTranslate(
                    "ReviewWorkspace", "Metadata is being prepared")
                : qsTranslate(
                    "ReviewWorkspace",
                    "No metadata is available for this photo")
        color: Theme.textQuiet
        font.pixelSize: 10
    }

    ShadowButton {
        visible: !exifSection.review.selectedHasMetadata
            && exifSection.review.controller.photoInspectionFailed
        text: qsTranslate("ReviewWorkspace", "Retry")
        variant: ShadowButton.Ghost
        enabled: !exifSection.review.controller.photoInspectionBusy
        onClicked:
            exifSection.review.controller.retryPhotoInspection()
    }

    Repeater {
        model: [
            { id: "captured_at", label: qsTranslate("ReviewWorkspace", "CAPTURED") },
            { id: "location", label: qsTranslate("ReviewWorkspace", "LOCATION") },
            { id: "camera", label: qsTranslate("ReviewWorkspace", "CAMERA") },
            { id: "lens", label: qsTranslate("ReviewWorkspace", "LENS") },
            { id: "exposure", label: qsTranslate("ReviewWorkspace", "SHUTTER") },
            { id: "aperture", label: qsTranslate("ReviewWorkspace", "APERTURE") },
            { id: "iso", label: qsTranslate("ReviewWorkspace", "SENSITIVITY") },
            { id: "focal_length", label: qsTranslate("ReviewWorkspace", "FOCAL LENGTH") },
            { id: "dimensions", label: qsTranslate("ReviewWorkspace", "PREVIEW") },
            { id: "focal_length_35mm", label: qsTranslate("ReviewWorkspace", "35 MM EQUIV.") },
            { id: "raw_dimensions", label: qsTranslate("ReviewWorkspace", "RAW SIZE") },
            { id: "sensor_bits", label: qsTranslate("ReviewWorkspace", "BIT DEPTH") },
            { id: "cfa", label: qsTranslate("ReviewWorkspace", "CFA") },
            { id: "dng", label: qsTranslate("ReviewWorkspace", "DNG") }
        ]

        delegate: RowLayout {
            id: exifRow

            required property var modelData

            Layout.fillWidth: true
            visible: exifSection.review.selectedHasMetadata
                && exifSection.review.preferences.exifFields.indexOf(
                    modelData.id) >= 0
            spacing: 8

            Label {
                Layout.preferredWidth: 76
                horizontalAlignment: Text.AlignRight
                text: exifRow.modelData.label
                color: exifSection.review.textMuted
                font.pixelSize: 9
            }

            Label {
                Layout.fillWidth: true
                text: exifSection.metadataPresentation.exifValue(
                    exifRow.modelData.id)
                color: exifSection.review.textPrimary
                font.pixelSize: 10
                elide: Text.ElideRight
            }
        }
    }
}
