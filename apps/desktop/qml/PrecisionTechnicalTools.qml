pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Detail recovery, optical correction and finishing effects evolve as one
// technical pipeline independently from the basic tone/color controls.
ColumnLayout {
    id: technical

    required property var inspector
    required property int currentTabIndex

    signal openOpticsProfileLibraryRequested()

    spacing: 8

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: technical.currentTabIndex === 0
        title: qsTr("DETAIL")
        toolTipText: qsTr("Control perceptual frequency detail, capture sharpening, and conventional noise reduction.")

        ShadowSubsectionLabel {
            text: qsTr("FREQUENCY DETAIL")
            toolTipText: qsTr("Clarity changes protected mid-frequency structure; Texture changes the smaller residual. Both operate only on Oklab L.")
        }

        Repeater {
            model: [
                { "key": "clarity", "name": qsTr("Clarity") },
                { "key": "texture", "name": qsTr("Texture") }
            ]
            delegate: ShadowSlider {
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                label: modelData.name
                from: -1.0
                to: 1.0
                neutralValue: 0.0
                stepSize: 0.01
                decimals: 0
                displayMultiplier: 100
                suffix: "%"
                value: inspector.fineValue(modelData.key)
                onGestureStarted: inspector.editor.beginParameterEdit(
                    modelData.key)
                onEdited: value => inspector.editor.setParameterValue(
                    modelData.key, value)
                onGestureFinished: inspector.editor.endParameterEdit(
                    modelData.key)
            }
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 6
            text: qsTr("SHARPENING")
            toolTipText: qsTr("Conventional capture sharpening. Use after frequency detail, and mask it to avoid sharpening smooth noise.")
        }

        Repeater {
            model: [
                { "key": "sharpen_amount", "name": qsTr("Amount"), "from": 0, "to": 2, "neutral": 0, "step": 0.01, "decimals": 0, "scale": 100, "suffix": "%" },
                { "key": "sharpen_radius", "name": qsTr("Radius"), "from": 0.1, "to": 5, "neutral": 1, "step": 0.1, "decimals": 1, "scale": 1, "suffix": " px" },
                { "key": "sharpen_threshold", "name": qsTr("Threshold"), "from": 0, "to": 1, "neutral": 0, "step": 0.01, "decimals": 0, "scale": 100, "suffix": "%" },
                { "key": "sharpen_masking", "name": qsTr("Masking"), "from": 0, "to": 1, "neutral": 0, "step": 0.01, "decimals": 0, "scale": 100, "suffix": "%" }
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
                value: inspector.fineValue(modelData.key)
                onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
            }
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 6
            text: qsTr("DENOISE")
            toolTipText: qsTr("Conventional RGB preview denoise. RAW-domain denoise is planned separately in the RAW development pipeline.")
        }

        Repeater {
            model: [
                { "key": "denoise_luminance", "name": qsTr("Luminance"), "neutral": 0 },
                { "key": "denoise_detail", "name": qsTr("Detail"), "neutral": 0.5 },
                { "key": "denoise_color", "name": qsTr("Color"), "neutral": 0 }
            ]
            delegate: ShadowSlider {
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                label: modelData.name
                from: 0; to: 1; neutralValue: modelData.neutral
                stepSize: 0.01; decimals: 0
                displayMultiplier: 100; suffix: "%"
                value: inspector.fineValue(modelData.key)
                onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
            }
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: technical.currentTabIndex === 0
        title: qsTr("OPTICS")

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 5

            ShadowSubsectionLabel {
                text: qsTr("PROFILE CORRECTION")
                toolTipText: qsTr("Lensfun supplies a calibrated baseline when a compatible camera and lens profile is available.")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    text: {
                    const receipt = inspector.editor.opticsReceipt
                    if (!receipt.valid)
                        return qsTr("Preparing lens profile…")
                    if (receipt.status === "matched")
                        return receipt.lensProfile.length > 0
                            ? receipt.lensProfile
                            : qsTr("Lens profile matched")
                    if (receipt.status === "disabled")
                        return qsTr("Automatic correction is bypassed")
                    if (receipt.status === "provider_unavailable")
                        return qsTr("Lensfun provider unavailable")
                    if (receipt.status === "camera_not_found")
                        return qsTr("Camera profile not found")
                    if (receipt.status === "lens_not_found")
                        return qsTr("Lens profile not found")
                    return qsTr("Insufficient lens metadata")
                    }
                    color: inspector.editor.opticsReceipt.status === "matched"
                        ? Theme.successText : Theme.textMuted
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
                Label {
                    visible: inspector.manualOpticsActive()
                    text: qsTr("Manual residual active")
                    color: inspector.accent
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                ShadowIconButton {
                    source: "qrc:/icons/library-manage.svg"
                    buttonSize: 26
                    toolTipText: qsTr("Choose optical profile")
                    accessibleName: toolTipText
                    onClicked: technical.openOpticsProfileLibraryRequested()
                }
            }

            Repeater {
                model: [
                    { "key": "master", "name": qsTr("Profile correction") },
                    { "key": "distortion", "name": qsTr("Distortion") },
                    { "key": "tca", "name": qsTr("Chromatic aberration") },
                    { "key": "vignetting", "name": qsTr("Lens vignetting") },
                    { "key": "scale", "name": qsTr("Automatic crop") }
                ]
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: 24
                    spacing: 8

                    readonly property bool optionChecked:
                        modelData.key === "master" ? inspector.editor.opticsEnabled
                        : modelData.key === "distortion" ? inspector.editor.opticsDistortionEnabled
                        : modelData.key === "tca" ? inspector.editor.opticsTcaEnabled
                        : modelData.key === "vignetting" ? inspector.editor.opticsVignettingEnabled
                        : inspector.editor.opticsAutomaticScale

                    Label {
                        Layout.fillWidth: true
                        text: parent.modelData.name
                        color: parent.enabled ? Theme.textSecondary : Theme.textDisabled
                        font.pixelSize: 10
                    }

                    Label {
                        readonly property bool applied:
                            inspector.opticsEffectState(
                                parent.modelData.key).startsWith(
                                    qsTr("Applied"))
                        text: inspector.opticsEffectState(parent.modelData.key)
                        color: applied ? Theme.successText : Theme.textMuted
                        font.pixelSize: 9
                        elide: Text.ElideRight
                        visible: text.length > 0
                    }

                    Switch {
                        id: opticsSwitch
                        Layout.preferredWidth: 34
                        Layout.preferredHeight: 20
                        checked: parent.optionChecked
                        enabled: inspector.editor.active
                            && !inspector.editor.stateBusy
                            && (parent.modelData.key === "master"
                                || inspector.editor.opticsEnabled)
                        onToggled: {
                            if (parent.modelData.key === "master")
                                inspector.editor.opticsEnabled = checked
                            else if (parent.modelData.key === "distortion")
                                inspector.editor.opticsDistortionEnabled = checked
                            else if (parent.modelData.key === "tca")
                                inspector.editor.opticsTcaEnabled = checked
                            else if (parent.modelData.key === "vignetting")
                                inspector.editor.opticsVignettingEnabled = checked
                            else
                                inspector.editor.opticsAutomaticScale = checked
                        }
                        indicator: Rectangle {
                            implicitWidth: 32
                            implicitHeight: 16
                            x: (opticsSwitch.width - width) / 2
                            y: (opticsSwitch.height - height) / 2
                            radius: height / 2
                            color: opticsSwitch.checked
                                ? Theme.switchOnSurface : Theme.switchOffSurface
                            border.color: opticsSwitch.checked
                                ? Theme.switchOnBorder : Theme.switchOffBorder
                            opacity: opticsSwitch.enabled ? 1 : 0.45
                            Rectangle {
                                width: 10; height: 10; y: 3
                                x: opticsSwitch.checked ? parent.width - width - 3 : 3
                                radius: width / 2
                                color: opticsSwitch.checked ? inspector.accent : inspector.textMuted
                            }
                        }
                        contentItem: Item {}
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.preferredHeight: 1
            color: Theme.border
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 4
            text: qsTr("MANUAL OPTICS")
            toolTipText: qsTr("Profile-independent residual correction. These controls stay available for manual lenses or images without a matching profile.")
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            text: qsTr("Applied after the selected Profile correction; works without a profile.")
            color: inspector.textMuted
            font.pixelSize: 9
            wrapMode: Text.WordWrap
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Distortion")
            toolTipText: qsTr("Residual radial geometry correction. Use after the Profile when straight lines still bow.")
            from: -100; to: 100; neutralValue: 0
            stepSize: 1; decimals: 0; suffix: "%"
            value: inspector.editor.manualOpticsDistortion
            onEdited: value => inspector.editor.manualOpticsDistortion = Math.round(value)
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 5
            text: qsTr("LATERAL CHROMATIC ABERRATION")
            toolTipText: qsTr("Geometrically realign color channels. This is distinct from purple/green Defringe below.")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Red / Cyan")
            toolTipText: qsTr("Move the red channel radially against green to correct red/cyan color fringes.")
            from: -100; to: 100; neutralValue: 0
            stepSize: 1; decimals: 0; suffix: "%"
            value: inspector.editor.manualOpticsTcaRedCyan
            onEdited: value => inspector.editor.manualOpticsTcaRedCyan = Math.round(value)
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Blue / Yellow")
            toolTipText: qsTr("Move the blue channel radially against green to correct blue/yellow color fringes.")
            from: -100; to: 100; neutralValue: 0
            stepSize: 1; decimals: 0; suffix: "%"
            value: inspector.editor.manualOpticsTcaBlueYellow
            onEdited: value => inspector.editor.manualOpticsTcaBlueYellow = Math.round(value)
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 5
            text: qsTr("OPTICAL VIGNETTING")
            toolTipText: qsTr("Lens shading correction in original optical coordinates. The creative post-crop vignette is in Looks.")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Amount")
            toolTipText: qsTr("Brighten or darken the outer lens shading before crop and creative grading.")
            from: -100; to: 100; neutralValue: 0
            stepSize: 1; decimals: 0; suffix: "%"
            value: inspector.editor.manualOpticsVignettingAmount
            onEdited: value => inspector.editor.manualOpticsVignettingAmount = Math.round(value)
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Midpoint")
            from: 0; to: 100; neutralValue: 50
            stepSize: 1; decimals: 0; suffix: "%"
            value: inspector.editor.manualOpticsVignettingMidpoint
            onEdited: value => inspector.editor.manualOpticsVignettingMidpoint = Math.round(value)
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 4
            text: qsTr("DEFRINGE")
            toolTipText: qsTr("Suppress purple and green chromatic fringes within the selected hue ranges.")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Purple amount")
            from: 0; to: 1; neutralValue: 0
            stepSize: 0.01; decimals: 0
            displayMultiplier: 100; suffix: "%"
            value: inspector.fineValue("defringe_purple_amount")
            onGestureStarted: inspector.editor.beginParameterEdit(
                "defringe_purple_amount")
            onEdited: value => inspector.editor.setParameterValue(
                "defringe_purple_amount", value)
            onGestureFinished: inspector.editor.endParameterEdit(
                "defringe_purple_amount")
        }

        ShadowHueRange {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Purple hue")
            accent: "#b66bd3"
            lowerValue: inspector.fineValue(
                "defringe_purple_hue_low")
            upperValue: inspector.fineValue(
                "defringe_purple_hue_high")
            onGestureStarted: inspector.editor.beginParameterEdit(
                "optics/defringe/purple/hue_range")
            onEdited: (lowerValue, upperValue) =>
                inspector.editor.setDefringeHueRange(
                    "purple", lowerValue, upperValue)
            onGestureFinished: inspector.editor.endParameterEdit(
                "optics/defringe/purple/hue_range")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Green amount")
            from: 0; to: 1; neutralValue: 0
            stepSize: 0.01; decimals: 0
            displayMultiplier: 100; suffix: "%"
            value: inspector.fineValue("defringe_green_amount")
            onGestureStarted: inspector.editor.beginParameterEdit(
                "defringe_green_amount")
            onEdited: value => inspector.editor.setParameterValue(
                "defringe_green_amount", value)
            onGestureFinished: inspector.editor.endParameterEdit(
                "defringe_green_amount")
        }

        ShadowHueRange {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Green hue")
            accent: "#58a66b"
            lowerValue: inspector.fineValue(
                "defringe_green_hue_low")
            upperValue: inspector.fineValue(
                "defringe_green_hue_high")
            onGestureStarted: inspector.editor.beginParameterEdit(
                "optics/defringe/green/hue_range")
            onEdited: (lowerValue, upperValue) =>
                inspector.editor.setDefringeHueRange(
                    "green", lowerValue, upperValue)
            onGestureFinished: inspector.editor.endParameterEdit(
                "optics/defringe/green/hue_range")
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: technical.currentTabIndex === 1
        title: qsTr("EFFECTS")

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Dehaze")
            from: -1; to: 1; neutralValue: 0
            stepSize: 0.01; decimals: 0
            displayMultiplier: 100; suffix: "%"
            value: inspector.fineValue("dehaze")
            onGestureStarted: inspector.editor.beginParameterEdit("dehaze")
            onEdited: value => inspector.editor.setParameterValue(
                "dehaze", value)
            onGestureFinished: inspector.editor.endParameterEdit("dehaze")
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 6
            text: qsTr("GRAIN")
            toolTipText: qsTr("Add a controlled photographic grain after the main color and tone adjustments.")
        }

        Repeater {
            model: [
                { "key": "grain_amount", "name": qsTr("Amount"), "neutral": 0 },
                { "key": "grain_size", "name": qsTr("Size"), "neutral": 0.5 },
                { "key": "grain_roughness", "name": qsTr("Roughness"), "neutral": 0.5 }
            ]
            delegate: ShadowSlider {
                required property var modelData
                Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14
                label: modelData.name; from: 0; to: 1
                neutralValue: modelData.neutral; stepSize: 0.01; decimals: 0
                displayMultiplier: 100; suffix: "%"
                value: inspector.fineValue(modelData.key)
                onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
            }
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 6
            text: qsTr("POST-CROP VIGNETTE")
            toolTipText: qsTr("Apply a creative vignette after cropping; this is separate from optical lens-vignetting correction.")
        }

        Repeater {
            model: [
                { "key": "vignette_amount", "name": qsTr("Amount"), "from": -1, "neutral": 0 },
                { "key": "vignette_midpoint", "name": qsTr("Midpoint"), "from": 0, "neutral": 0.5 },
                { "key": "vignette_roundness", "name": qsTr("Roundness"), "from": -1, "neutral": 0 },
                { "key": "vignette_feather", "name": qsTr("Feather"), "from": 0, "neutral": 0.5 },
                { "key": "vignette_highlights", "name": qsTr("Highlights"), "from": 0, "neutral": 0 }
            ]
            delegate: ShadowSlider {
                required property var modelData
                Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14
                label: modelData.name; from: modelData.from; to: 1
                neutralValue: modelData.neutral; stepSize: 0.01; decimals: 0
                displayMultiplier: 100; suffix: "%"
                value: inspector.fineValue(modelData.key)
                onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
            }
        }
    }
}
