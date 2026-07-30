pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// Finishing grain and post-crop vignette share the final creative-effects
// stage while remaining independent from optical lens correction.
ShadowAdjustmentSection {
    id: effectsSection

    required property var inspector

    title: qsTranslate("PrecisionWorkspace", "EFFECTS")
    resetAvailable: true
    onResetRequested:
        inspector.editor.resetSelectedAdjustmentSection("effects")

    ShadowSubsectionLabel {
        Layout.topMargin: 0
        text: qsTranslate("PrecisionWorkspace", "GRAIN")
        toolTipText: qsTranslate(
            "PrecisionWorkspace",
            "Add a controlled photographic grain after the main color and tone adjustments.")
    }

    Repeater {
        model: [
            { "key": "grain_amount", "name": qsTranslate("PrecisionWorkspace", "Amount"), "neutral": 0 },
            { "key": "grain_size", "name": qsTranslate("PrecisionWorkspace", "Size"), "neutral": 0.5 },
            { "key": "grain_roughness", "name": qsTranslate("PrecisionWorkspace", "Roughness"), "neutral": 0.5 }
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
            value: effectsSection.inspector.fineValue(modelData.key)
            onGestureStarted:
                effectsSection.inspector.editor.beginParameterEdit(modelData.key)
            onEdited: value =>
                effectsSection.inspector.editor.setParameterValue(
                    modelData.key, value)
            onGestureFinished:
                effectsSection.inspector.editor.endParameterEdit(modelData.key)
        }
    }

    ShadowSubsectionLabel {
        Layout.topMargin: 6
        text: qsTranslate("PrecisionWorkspace", "POST-CROP VIGNETTE")
        toolTipText: qsTranslate(
            "PrecisionWorkspace",
            "Apply a creative vignette after cropping; this is separate from optical lens-vignetting correction.")
    }

    Repeater {
        model: [
            { "key": "vignette_amount", "name": qsTranslate("PrecisionWorkspace", "Amount"), "from": -1, "neutral": 0 },
            { "key": "vignette_midpoint", "name": qsTranslate("PrecisionWorkspace", "Midpoint"), "from": 0, "neutral": 0.5 },
            { "key": "vignette_roundness", "name": qsTranslate("PrecisionWorkspace", "Roundness"), "from": -1, "neutral": 0 },
            { "key": "vignette_feather", "name": qsTranslate("PrecisionWorkspace", "Feather"), "from": 0, "neutral": 0.5 },
            { "key": "vignette_highlights", "name": qsTranslate("PrecisionWorkspace", "Highlights"), "from": 0, "neutral": 0 }
        ]
        delegate: ShadowSlider {
            required property var modelData
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: modelData.name
            from: modelData.from
            to: 1
            neutralValue: modelData.neutral
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: effectsSection.inspector.fineValue(modelData.key)
            onGestureStarted:
                effectsSection.inspector.editor.beginParameterEdit(modelData.key)
            onEdited: value =>
                effectsSection.inspector.editor.setParameterValue(
                    modelData.key, value)
            onGestureFinished:
                effectsSection.inspector.editor.endParameterEdit(modelData.key)
        }
    }
}
