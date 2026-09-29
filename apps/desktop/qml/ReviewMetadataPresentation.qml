pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick

QtObject {
    id: presentation

    required property var workspace

    // JS helper calls can inherit the caller context; keep message identity explicit.
    function formatLuma(value) {
        return Number(value).toLocaleString(Qt.locale(), "f", 3)
    }

    function formatPercent(value) {
        return qsTranslate("ReviewWorkspace", "%1%").arg(
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
            return qsTranslate("ReviewWorkspace", "%1 s").arg(value.toLocaleString(Qt.locale(), "f", value < 10 ? 1 : 0))
        const reciprocal = Math.round(1 / value)
        return reciprocal > 1 ? qsTranslate("ReviewWorkspace", "1/%1 s").arg(reciprocal)
                              : qsTranslate("ReviewWorkspace", "%1 s").arg(value.toLocaleString(Qt.locale(), "f", 2))
    }

    function exifValue(field) {
        switch (field) {
        case "captured_at":
            return Number(presentation.workspace.selectedCapturedAtUnixSeconds) > 0
                ? new Date(
                    Number(presentation.workspace.selectedCapturedAtUnixSeconds) * 1000
                ).toLocaleString(Qt.locale(), Locale.ShortFormat) : "—"
        case "location":
            if (!presentation.workspace.selectedHasCoordinates)
                return "—"
            const coordinates = qsTranslate("ReviewWorkspace", "%1, %2").arg(
                presentation.workspace.selectedLatitude.toLocaleString(
                    Qt.locale(), "f", 6)).arg(
                presentation.workspace.selectedLongitude.toLocaleString(
                    Qt.locale(), "f", 6))
            const placeName = presentation.workspace.selectedPlaceName.length > 0
                ? presentation.workspace.selectedPlaceName
                : presentation.workspace.selectedResolvedPlaceName
            return placeName.length > 0
                ? placeName + " · " + coordinates
                : coordinates
        case "camera": return joinedIdentity(presentation.workspace.selectedCameraMake, presentation.workspace.selectedCameraModel)
        case "lens": return joinedIdentity(presentation.workspace.selectedLensMake, presentation.workspace.selectedLensModel)
        case "exposure": return formatShutter(presentation.workspace.selectedExposureTimeSeconds)
        case "aperture": return presentation.workspace.selectedApertureFNumber > 0
            ? qsTranslate("ReviewWorkspace", "f/%1").arg(presentation.workspace.selectedApertureFNumber.toLocaleString(Qt.locale(), "f", 1)) : "—"
        case "iso": return presentation.workspace.selectedIsoSpeed > 0 ? qsTranslate("ReviewWorkspace", "ISO %1").arg(Math.round(presentation.workspace.selectedIsoSpeed)) : "—"
        case "focal_length": return presentation.workspace.selectedFocalLengthMm > 0
            ? qsTranslate("ReviewWorkspace", "%1 mm").arg(presentation.workspace.selectedFocalLengthMm.toLocaleString(Qt.locale(), "f", 1)) : "—"
        case "dimensions": return presentation.workspace.selectedWidth > 0 ? qsTranslate("ReviewWorkspace", "%L1 × %L2").arg(presentation.workspace.selectedWidth).arg(presentation.workspace.selectedHeight) : "—"
        case "focal_length_35mm": return presentation.workspace.selectedFocalLength35mm > 0
            ? qsTranslate("ReviewWorkspace", "%1 mm equiv.").arg(presentation.workspace.selectedFocalLength35mm.toLocaleString(Qt.locale(), "f", 0)) : "—"
        case "raw_dimensions": return presentation.workspace.selectedRawWidth > 0 ? qsTranslate("ReviewWorkspace", "%L1 × %L2").arg(presentation.workspace.selectedRawWidth).arg(presentation.workspace.selectedRawHeight) : "—"
        case "sensor_bits": return presentation.workspace.selectedSensorBits > 0 ? qsTranslate("ReviewWorkspace", "%1-bit").arg(presentation.workspace.selectedSensorBits) : "—"
        case "cfa": return presentation.workspace.selectedCfaPattern.length > 0 ? presentation.workspace.selectedCfaPattern : "—"
        case "dng": return presentation.workspace.selectedDngVersion.length > 0 ? presentation.workspace.selectedDngVersion : "—"
        default: return "—"
        }
    }

    function metadataFields() {
        return [
            { id: "captured_at", group: qsTranslate("ReviewWorkspace", "Capture"), firstInGroup: true,
              label: qsTranslate("ReviewWorkspace", "Capture time"), value: exifValue("captured_at") },
            { id: "exposure", group: qsTranslate("ReviewWorkspace", "Capture"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "Shutter speed"), value: exifValue("exposure") },
            { id: "aperture", group: qsTranslate("ReviewWorkspace", "Capture"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "Aperture"), value: exifValue("aperture") },
            { id: "iso", group: qsTranslate("ReviewWorkspace", "Capture"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "ISO sensitivity"), value: exifValue("iso") },
            { id: "location", group: qsTranslate("ReviewWorkspace", "Capture"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "Location"), value: exifValue("location") },
            { id: "camera", group: qsTranslate("ReviewWorkspace", "Camera and lens"), firstInGroup: true,
              label: qsTranslate("ReviewWorkspace", "Camera"), value: exifValue("camera") },
            { id: "lens", group: qsTranslate("ReviewWorkspace", "Camera and lens"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "Lens"), value: exifValue("lens") },
            { id: "focal_length", group: qsTranslate("ReviewWorkspace", "Camera and lens"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "Focal length"), value: exifValue("focal_length") },
            { id: "focal_length_35mm", group: qsTranslate("ReviewWorkspace", "Camera and lens"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "35 mm equivalent"), value: exifValue("focal_length_35mm") },
            { id: "dimensions", group: qsTranslate("ReviewWorkspace", "Image"), firstInGroup: true,
              label: qsTranslate("ReviewWorkspace", "Preview dimensions"), value: exifValue("dimensions") },
            { id: "raw_dimensions", group: qsTranslate("ReviewWorkspace", "Image"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "RAW dimensions"), value: exifValue("raw_dimensions") },
            { id: "sensor_bits", group: qsTranslate("ReviewWorkspace", "Image"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "Sensor bit depth"), value: exifValue("sensor_bits") },
            { id: "cfa", group: qsTranslate("ReviewWorkspace", "Image"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "Color filter array"), value: exifValue("cfa") },
            { id: "dng", group: qsTranslate("ReviewWorkspace", "Image"), firstInGroup: false,
              label: qsTranslate("ReviewWorkspace", "DNG version"), value: exifValue("dng") }
        ]
    }
}
