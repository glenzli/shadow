pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// Capture sharpening and conventional noise reduction share one detail-recovery
// contract and one parameter-gesture lifecycle.
ShadowAdjustmentSection {
    id: detailSection

    required property var inspector

    title: qsTranslate("PrecisionWorkspace", "DETAIL")
    toolTipText: qsTranslate(
        "PrecisionWorkspace",
        "Control capture sharpening and conventional noise reduction.")

    ShadowSubsectionLabel {
        Layout.topMargin: 6
        text: qsTranslate("PrecisionWorkspace", "SHARPENING")
        toolTipText: qsTranslate(
            "PrecisionWorkspace",
            "Conventional capture sharpening. Use after frequency detail, and mask it to avoid sharpening smooth noise.")
    }

    Repeater {
        model: [
            { "key": "sharpen_amount", "name": qsTranslate("PrecisionWorkspace", "Amount"), "from": 0, "to": 2, "neutral": 0, "step": 0.01, "decimals": 0, "scale": 100, "suffix": "%" },
            { "key": "sharpen_radius", "name": qsTranslate("PrecisionWorkspace", "Radius"), "from": 0.1, "to": 5, "neutral": 1, "step": 0.1, "decimals": 1, "scale": 1, "suffix": " px" },
            { "key": "sharpen_threshold", "name": qsTranslate("PrecisionWorkspace", "Threshold"), "from": 0, "to": 1, "neutral": 0, "step": 0.01, "decimals": 0, "scale": 100, "suffix": "%" },
            { "key": "sharpen_masking", "name": qsTranslate("PrecisionWorkspace", "Masking"), "from": 0, "to": 1, "neutral": 0, "step": 0.01, "decimals": 0, "scale": 100, "suffix": "%" }
        ]
        delegate: ShadowSlider {
            required property var modelData
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: modelData.name
            from: modelData.from
            to: modelData.to
            neutralValue: modelData.neutral
            stepSize: modelData.step
            decimals: modelData.decimals
            displayMultiplier: modelData.scale
            suffix: modelData.suffix
            value: detailSection.inspector.fineValue(modelData.key)
            onGestureStarted:
                detailSection.inspector.editor.beginParameterEdit(modelData.key)
            onEdited: value =>
                detailSection.inspector.editor.setParameterValue(
                    modelData.key, value)
            onGestureFinished:
                detailSection.inspector.editor.endParameterEdit(modelData.key)
        }
    }

    ShadowSubsectionLabel {
        Layout.topMargin: 6
        text: qsTranslate("PrecisionWorkspace", "DENOISE")
        toolTipText: qsTranslate(
            "PrecisionWorkspace",
            "The controls below denoise the current RGB preview immediately with separate luminance and color passes. RAW files can additionally use a conservative sensor-domain pass for full-size processing and export; JPEG and HEIF retain the same responsive RGB controls.")
    }

    Repeater {
        model: [
            { "key": "denoise_luminance", "name": qsTranslate("PrecisionWorkspace", "Luminance"), "neutral": 0 },
            { "key": "denoise_detail", "name": qsTranslate("PrecisionWorkspace", "Detail"), "neutral": 0.5 },
            { "key": "denoise_color", "name": qsTranslate("PrecisionWorkspace", "Color"), "neutral": 0 }
        ]
        delegate: ShadowSlider {
            required property var modelData
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: modelData.name
            from: 0
            to: 1
            neutralValue: modelData.neutral
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: detailSection.inspector.fineValue(modelData.key)
            onGestureStarted:
                detailSection.inspector.editor.beginParameterEdit(modelData.key)
            onEdited: value =>
                detailSection.inspector.editor.setParameterValue(
                    modelData.key, value)
            onGestureFinished:
                detailSection.inspector.editor.endParameterEdit(modelData.key)
        }
    }
}
