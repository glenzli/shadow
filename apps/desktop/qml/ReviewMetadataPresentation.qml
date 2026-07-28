pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick

QtObject {
    id: presentation

    required property var workspace

    function formatLuma(value) {
        return Number(value).toLocaleString(Qt.locale(), "f", 3)
    }

    function formatPercent(value) {
        return qsTr("%1%").arg(
            (Number(value) * 100.0).toLocaleString(Qt.locale(), "f", 2))
    }

    function formatProxyDetail(value) {
        const number = Number(value)
        const magnitude = Math.abs(number)
        if (magnitude > 0.0 && (magnitude < 0.001 || magnitude >= 1000.0))
            return number.toLocaleString(Qt.locale(), "e", 3)
        return number.toLocaleString(Qt.locale(), "f", 4)
    }

    function concisePreprocessingVersion(value) {
        const parts = String(value).split(":")
        if (parts.length < 2)
            return value.length > 0 ? value : "—"
        return parts[0] + " · " + parts[parts.length - 1]
    }

    function joinedIdentity(make, model) {
        const parts = []
        if (String(make).trim().length > 0)
            parts.push(String(make).trim())
        if (String(model).trim().length > 0
                && String(model).trim() !== String(make).trim())
            parts.push(String(model).trim())
        return parts.length > 0 ? parts.join(" ") : "—"
    }

    function formatShutter(seconds) {
        const value = Number(seconds)
        if (!(value > 0))
            return "—"
        if (value >= 1)
            return qsTr("%1 s").arg(value.toLocaleString(Qt.locale(), "f", value < 10 ? 1 : 0))
        const reciprocal = Math.round(1 / value)
        return reciprocal > 1 ? qsTr("1/%1 s").arg(reciprocal)
                              : qsTr("%1 s").arg(value.toLocaleString(Qt.locale(), "f", 2))
    }

    function exifValue(field) {
        switch (field) {
        case "captured_at":
            return Number(presentation.workspace.selectedCapturedAtUnixSeconds) > 0
                ? new Date(Number(presentation.workspace.selectedCapturedAtUnixSeconds) * 1000).toLocaleString(Qt.locale()) : "—"
        case "camera": return joinedIdentity(presentation.workspace.selectedCameraMake, presentation.workspace.selectedCameraModel)
        case "lens": return joinedIdentity(presentation.workspace.selectedLensMake, presentation.workspace.selectedLensModel)
        case "exposure": return formatShutter(presentation.workspace.selectedExposureTimeSeconds)
        case "aperture": return presentation.workspace.selectedApertureFNumber > 0
            ? qsTr("f/%1").arg(presentation.workspace.selectedApertureFNumber.toLocaleString(Qt.locale(), "f", 1)) : "—"
        case "iso": return presentation.workspace.selectedIsoSpeed > 0 ? qsTr("ISO %1").arg(Math.round(presentation.workspace.selectedIsoSpeed)) : "—"
        case "focal_length": return presentation.workspace.selectedFocalLengthMm > 0
            ? qsTr("%1 mm").arg(presentation.workspace.selectedFocalLengthMm.toLocaleString(Qt.locale(), "f", 1)) : "—"
        case "dimensions": return presentation.workspace.selectedWidth > 0 ? qsTr("%L1 × %L2").arg(presentation.workspace.selectedWidth).arg(presentation.workspace.selectedHeight) : "—"
        case "focal_length_35mm": return presentation.workspace.selectedFocalLength35mm > 0
            ? qsTr("%1 mm equiv.").arg(presentation.workspace.selectedFocalLength35mm.toLocaleString(Qt.locale(), "f", 0)) : "—"
        case "raw_dimensions": return presentation.workspace.selectedRawWidth > 0 ? qsTr("%L1 × %L2").arg(presentation.workspace.selectedRawWidth).arg(presentation.workspace.selectedRawHeight) : "—"
        case "sensor_bits": return presentation.workspace.selectedSensorBits > 0 ? qsTr("%1-bit").arg(presentation.workspace.selectedSensorBits) : "—"
        case "cfa": return presentation.workspace.selectedCfaPattern.length > 0 ? presentation.workspace.selectedCfaPattern : "—"
        case "dng": return presentation.workspace.selectedDngVersion.length > 0 ? presentation.workspace.selectedDngVersion : "—"
        default: return "—"
        }
    }

    function metadataFields() {
        return [
            { id: "captured_at", group: qsTr("Capture"), firstInGroup: true,
              label: qsTr("Capture time"), value: exifValue("captured_at") },
            { id: "exposure", group: qsTr("Capture"), firstInGroup: false,
              label: qsTr("Shutter speed"), value: exifValue("exposure") },
            { id: "aperture", group: qsTr("Capture"), firstInGroup: false,
              label: qsTr("Aperture"), value: exifValue("aperture") },
            { id: "iso", group: qsTr("Capture"), firstInGroup: false,
              label: qsTr("ISO sensitivity"), value: exifValue("iso") },
            { id: "camera", group: qsTr("Camera and lens"), firstInGroup: true,
              label: qsTr("Camera"), value: exifValue("camera") },
            { id: "lens", group: qsTr("Camera and lens"), firstInGroup: false,
              label: qsTr("Lens"), value: exifValue("lens") },
            { id: "focal_length", group: qsTr("Camera and lens"), firstInGroup: false,
              label: qsTr("Focal length"), value: exifValue("focal_length") },
            { id: "focal_length_35mm", group: qsTr("Camera and lens"), firstInGroup: false,
              label: qsTr("35 mm equivalent"), value: exifValue("focal_length_35mm") },
            { id: "dimensions", group: qsTr("Image"), firstInGroup: true,
              label: qsTr("Preview dimensions"), value: exifValue("dimensions") },
            { id: "raw_dimensions", group: qsTr("Image"), firstInGroup: false,
              label: qsTr("RAW dimensions"), value: exifValue("raw_dimensions") },
            { id: "sensor_bits", group: qsTr("Image"), firstInGroup: false,
              label: qsTr("Sensor bit depth"), value: exifValue("sensor_bits") },
            { id: "cfa", group: qsTr("Image"), firstInGroup: false,
              label: qsTr("Color filter array"), value: exifValue("cfa") },
            { id: "dng", group: qsTr("Image"), firstInGroup: false,
              label: qsTr("DNG version"), value: exifValue("dng") }
        ]
    }
}
