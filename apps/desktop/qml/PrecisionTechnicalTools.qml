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
    property int opticsTabIndex: 0

    signal openOpticsProfileLibraryRequested()

    spacing: 8

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: technical.currentTabIndex === 0
        title: qsTr("DETAIL")
        toolTipText: qsTr("Control capture sharpening and conventional noise reduction.")

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
            toolTipText: qsTr("The controls below denoise the current RGB preview immediately with separate luminance and color passes. RAW files can additionally use a conservative sensor-domain pass for full-size processing and export; JPEG and HEIF retain the same responsive RGB controls.")
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

        TabBar {
            id: opticsTabs

            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.preferredHeight: 32
            currentIndex: technical.opticsTabIndex
            background: Item {}
            onCurrentIndexChanged: technical.opticsTabIndex = currentIndex

            ShadowTabButton {
                text: qsTr("PROFILE")
                compact: true
                underlineInset: 20
                toolTipText: qsTr("Automatic lens-profile correction with an optional profile override")
            }
            ShadowTabButton {
                text: qsTr("MANUAL")
                compact: true
                underlineInset: 20
                toolTipText: qsTr("Residual geometry, vignetting, and color-fringe correction")
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 4
            visible: technical.opticsTabIndex === 0
            spacing: 8

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: opticsCardContent.implicitHeight + 20
                radius: 8
                color: Theme.surfaceSubtle
                border.width: 1
                border.color: inspector.editor.opticsReceipt.status === "matched"
                    ? Theme.accentBorder : inspector.panelBorder

                ColumnLayout {
                    id: opticsCardContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            radius: 4
                            color: inspector.editor.opticsReceipt.status === "matched"
                                ? Theme.successText
                                : inspector.editor.opticsManualProfile
                                    ? inspector.accent : Theme.textMuted
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1

                            Label {
                                text: !inspector.editor.opticsEnabled
                                    ? qsTr("NO PROFILE")
                                    : inspector.editor.opticsManualProfile
                                        ? qsTr("PROFILE OVERRIDE")
                                        : qsTr("AUTOMATIC PROFILE")
                                color: !inspector.editor.opticsEnabled
                                    ? Theme.textMuted : inspector.editor.opticsManualProfile
                                        ? inspector.accent : Theme.textSecondary
                                font.pixelSize: 8
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.45
                            }

                            Label {
                                id: activeOpticsProfileLabel
                                Layout.fillWidth: true
                                text: {
                                    const receipt = inspector.editor.opticsReceipt
                                    if (!inspector.editor.opticsEnabled)
                                        return qsTr("No profile correction")
                                    if (!receipt.valid)
                                        return qsTr("Preparing lens profile…")
                                    if (receipt.status === "matched")
                                        return receipt.lensProfile.length > 0
                                            ? receipt.lensProfile
                                            : qsTr("Lens profile matched")
                                    if (receipt.status === "disabled")
                                        return qsTr("Profile correction disabled")
                                    if (receipt.status === "provider_unavailable")
                                        return qsTr("Lensfun unavailable")
                                    if (receipt.status === "camera_not_found")
                                        return qsTr("Camera not found")
                                    if (receipt.status === "lens_not_found")
                                        return qsTr("Lens not found")
                                    return qsTr("Lens metadata unavailable")
                                }
                                color: inspector.editor.opticsEnabled
                                    && inspector.editor.opticsReceipt.status === "matched"
                                    ? Theme.textPrimary : Theme.textMuted
                                font.pixelSize: 10
                                font.weight: Font.Medium
                                elide: Text.ElideRight

                                HoverHandler { id: activeOpticsProfileHover }
                                ToolTip.visible: activeOpticsProfileHover.hovered
                                    && activeOpticsProfileLabel.truncated
                                ToolTip.delay: 500
                                ToolTip.text: activeOpticsProfileLabel.text
                            }
                        }

                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: inspector.panelBorder
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: {
                                const receipt = inspector.editor.opticsReceipt
                                return receipt.cameraProfile
                                    && receipt.cameraProfile.length > 0
                                    ? receipt.cameraProfile
                                    : qsTr("EXIF camera and lens matching")
                            }
                            color: Theme.textMuted
                            font.pixelSize: 9
                            elide: Text.ElideRight
                        }

                        ShadowIconButton {
                            source: "qrc:/icons/library-manage.svg"
                            buttonSize: 26
                            toolTipText: qsTr("Choose or override lens profile")
                            accessibleName: toolTipText
                            onClicked:
                                technical.openOpticsProfileLibraryRequested()
                        }

                        ShadowIconButton {
                            visible: inspector.editor.opticsEnabled
                                && inspector.editor.opticsManualProfile
                            source: "qrc:/icons/clear.svg"
                            buttonSize: 26
                            toolTipText: qsTr("Return to automatic matching")
                            accessibleName: toolTipText
                            onClicked:
                                inspector.editor.clearManualOpticsProfile()
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: {
                    const receipt = inspector.editor.opticsReceipt
                    if (!inspector.editor.opticsEnabled)
                        return qsTr("No profile correction is applied.")
                    if (receipt.status === "matched")
                        return qsTr("All calibrated corrections from this profile are applied together.")
                    return qsTr("No matching profile correction is available for this photo.")
                }
                color: inspector.editor.opticsEnabled
                    && inspector.editor.opticsReceipt.status === "matched"
                    ? Theme.textSecondary : Theme.textMuted
                font.pixelSize: 9
                wrapMode: Text.WordWrap
                ToolTip.visible: opticsProfileSummaryHover.hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("A profile is one coherent correction. Distortion, lateral chromatic aberration, lens shading, and automatic crop use every calibrated record that Lensfun provides; residual adjustments live in Manual.")
                HoverHandler { id: opticsProfileSummaryHover }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            Layout.topMargin: 4
            visible: technical.opticsTabIndex === 1
            spacing: 0

            ShadowSlider {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                label: qsTr("Distortion")
                toolTipText: qsTr("Residual radial geometry correction applied after the selected profile.")
                from: -100; to: 100; neutralValue: 0
                stepSize: 1; decimals: 0; suffix: "%"
                value: inspector.editor.manualOpticsDistortion
                onEdited: value => inspector.editor.manualOpticsDistortion = Math.round(value)
            }

            ShadowSubsectionLabel {
                Layout.topMargin: 5
                text: qsTr("CHROMATIC ABERRATION")
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
                text: qsTr("VIGNETTING")
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
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: technical.currentTabIndex === 1
        title: qsTr("EFFECTS")

        ShadowSubsectionLabel {
            Layout.topMargin: 0
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
