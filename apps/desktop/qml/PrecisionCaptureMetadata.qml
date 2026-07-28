pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns capture identity/setting formatting and its compact Precision presentation.
Rectangle {
    id: metadata

    required property var captureMetadata
    required property string representationId
    required property color panelColor
    required property color borderColor
    required property color textPrimary
    required property color textSecondary
    required property color textMuted

    readonly property bool metadataMatches:
        captureMetadata
        && captureMetadata.representationId === representationId
    readonly property bool metadataAvailable:
        metadataMatches && captureMetadata.available

    color: panelColor

    function joinedIdentity(make, model) {
        const parts = []
        const cleanMake = String(make || "").trim()
        const cleanModel = String(model || "").trim()
        if (cleanMake.length > 0)
            parts.push(cleanMake)
        if (cleanModel.length > 0 && cleanModel !== cleanMake)
            parts.push(cleanModel)
        return parts.join(" ")
    }

    function formatShutter(seconds) {
        const value = Number(seconds)
        if (!(value > 0))
            return "—"
        if (value >= 1)
            return qsTranslate("PrecisionWorkspace", "%1 s").arg(
                value.toLocaleString(Qt.locale(), "f", value < 10 ? 1 : 0))
        const reciprocal = Math.round(1 / value)
        return reciprocal > 1
            ? qsTranslate("PrecisionWorkspace", "1/%1 s").arg(reciprocal)
            : qsTranslate("PrecisionWorkspace", "%1 s").arg(
                value.toLocaleString(Qt.locale(), "f", 2))
    }

    function captureSettingSummary() {
        if (!captureMetadata || !captureMetadata.available)
            return ""
        const values = []
        values.push(formatShutter(captureMetadata.exposureTimeSeconds))
        values.push(Number(captureMetadata.apertureFNumber) > 0
            ? qsTranslate("PrecisionWorkspace", "f/%1").arg(
                Number(captureMetadata.apertureFNumber)
                .toLocaleString(Qt.locale(), "f", 1)) : "—")
        values.push(Number(captureMetadata.isoSpeed) > 0
            ? qsTranslate("PrecisionWorkspace", "ISO %1").arg(Math.round(
                Number(captureMetadata.isoSpeed))) : "—")
        values.push(Number(captureMetadata.focalLengthMm) > 0
            ? qsTranslate("PrecisionWorkspace", "%1 mm").arg(
                Number(captureMetadata.focalLengthMm)
                .toLocaleString(Qt.locale(), "f", 1)) : "—")
        return values.join("   ·   ")
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: metadata.borderColor
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 4

        Label {
            objectName: "precisionCaptureMetadataText"
            width: parent.width
            text: metadata.metadataAvailable
                ? metadata.joinedIdentity(
                    metadata.captureMetadata.cameraMake,
                    metadata.captureMetadata.cameraModel)
                : metadata.metadataMatches
                    && metadata.captureMetadata.pending
                    ? qsTranslate(
                        "PrecisionWorkspace",
                        "Preparing capture metadata…")
                    : qsTranslate(
                        "PrecisionWorkspace",
                        "Capture metadata unavailable")
            color: metadata.metadataAvailable
                ? metadata.textPrimary : metadata.textMuted
            font.pixelSize: 10
            font.weight: metadata.metadataAvailable
                ? Font.Medium : Font.Normal
            elide: Text.ElideRight
        }

        RowLayout {
            width: parent.width
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: metadata.metadataAvailable
                    ? metadata.captureSettingSummary() : ""
                color: metadata.textSecondary
                font.pixelSize: 9
                elide: Text.ElideRight
            }

            Label {
                Layout.maximumWidth: parent.width * 0.42
                visible: metadata.metadataAvailable && text.length > 0
                text: metadata.joinedIdentity(
                    metadata.captureMetadata.lensMake,
                    metadata.captureMetadata.lensModel)
                color: metadata.textMuted
                font.pixelSize: 9
                elide: Text.ElideRight
            }
        }
    }
}
