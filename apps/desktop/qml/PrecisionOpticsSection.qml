pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Profile matching and residual manual correction form one optical-correction
// owner. The profile-library request is the only intent exposed upward.
ShadowAdjustmentSection {
    id: opticsSection

    required property var inspector
    property int currentTabIndex: 0

    signal openProfileLibraryRequested()

    title: qsTranslate("PrecisionWorkspace", "OPTICS")

    TabBar {
        id: opticsTabs

        Layout.fillWidth: true
        Layout.leftMargin: 18
        Layout.rightMargin: 18
        Layout.preferredHeight: 32
        currentIndex: opticsSection.currentTabIndex
        background: Item {}
        onCurrentIndexChanged: opticsSection.currentTabIndex = currentIndex

        ShadowTabButton {
            text: qsTranslate("PrecisionWorkspace", "PROFILE")
            compact: true
            underlineInset: 20
            toolTipText: qsTranslate(
                "PrecisionWorkspace",
                "Automatic lens-profile correction with an optional profile override")
        }
        ShadowTabButton {
            text: qsTranslate("PrecisionWorkspace", "MANUAL")
            compact: true
            underlineInset: 20
            toolTipText: qsTranslate(
                "PrecisionWorkspace",
                "Residual geometry, vignetting, and color-fringe correction")
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? implicitHeight : 0
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.topMargin: 4
        visible: opticsSection.currentTabIndex === 0
        spacing: 8

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: opticsCardContent.implicitHeight + 20
            radius: 8
            color: Theme.surfaceSubtle
            border.width: 1
            border.color:
                opticsSection.inspector.editor.opticsReceipt.status === "matched"
                ? Theme.accentBorder : opticsSection.inspector.panelBorder

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
                        color:
                            opticsSection.inspector.editor.opticsReceipt.status
                                === "matched"
                            ? Theme.successText
                            : opticsSection.inspector.editor.opticsManualProfile
                                ? opticsSection.inspector.accent : Theme.textMuted
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1

                        Label {
                            text: !opticsSection.inspector.editor.opticsEnabled
                                ? qsTranslate("PrecisionWorkspace", "NO PROFILE")
                                : opticsSection.inspector.editor.opticsManualProfile
                                    ? qsTranslate(
                                        "PrecisionWorkspace",
                                        "PROFILE OVERRIDE")
                                    : qsTranslate(
                                        "PrecisionWorkspace",
                                        "AUTOMATIC PROFILE")
                            color:
                                !opticsSection.inspector.editor.opticsEnabled
                                ? Theme.textMuted
                                : opticsSection.inspector.editor.opticsManualProfile
                                    ? opticsSection.inspector.accent
                                    : Theme.textSecondary
                            font.pixelSize: 8
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.45
                        }

                        Label {
                            id: activeOpticsProfileLabel
                            Layout.fillWidth: true
                            text: {
                                const receipt =
                                    opticsSection.inspector.editor.opticsReceipt
                                if (!opticsSection.inspector.editor.opticsEnabled)
                                    return qsTranslate(
                                        "PrecisionWorkspace",
                                        "No profile correction")
                                if (!receipt.valid)
                                    return qsTranslate(
                                        "PrecisionWorkspace",
                                        "Preparing lens profile…")
                                if (receipt.status === "matched")
                                    return receipt.lensProfile.length > 0
                                        ? receipt.lensProfile
                                        : qsTranslate(
                                            "PrecisionWorkspace",
                                            "Lens profile matched")
                                if (receipt.status === "disabled")
                                    return qsTranslate(
                                        "PrecisionWorkspace",
                                        "Profile correction disabled")
                                if (receipt.status === "provider_unavailable")
                                    return qsTranslate(
                                        "PrecisionWorkspace",
                                        "Lensfun unavailable")
                                if (receipt.status === "camera_not_found")
                                    return qsTranslate(
                                        "PrecisionWorkspace",
                                        "Camera not found")
                                if (receipt.status === "lens_not_found")
                                    return qsTranslate(
                                        "PrecisionWorkspace",
                                        "Lens not found")
                                return qsTranslate(
                                    "PrecisionWorkspace",
                                    "Lens metadata unavailable")
                            }
                            color:
                                opticsSection.inspector.editor.opticsEnabled
                                && opticsSection.inspector.editor.opticsReceipt.status
                                    === "matched"
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
                    color: opticsSection.inspector.panelBorder
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        text: {
                            const receipt =
                                opticsSection.inspector.editor.opticsReceipt
                            return receipt.cameraProfile
                                && receipt.cameraProfile.length > 0
                                ? receipt.cameraProfile
                                : qsTranslate(
                                    "PrecisionWorkspace",
                                    "EXIF camera and lens matching")
                        }
                        color: Theme.textMuted
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/library-manage.svg"
                        buttonSize: 26
                        toolTipText: qsTranslate(
                            "PrecisionWorkspace",
                            "Choose or override lens profile")
                        accessibleName: toolTipText
                        onClicked:
                            opticsSection.openProfileLibraryRequested()
                    }

                    ShadowIconButton {
                        visible: opticsSection.inspector.editor.opticsEnabled
                            && opticsSection.inspector.editor.opticsManualProfile
                        source: "qrc:/icons/clear.svg"
                        buttonSize: 26
                        toolTipText: qsTranslate(
                            "PrecisionWorkspace",
                            "Return to automatic matching")
                        accessibleName: toolTipText
                        onClicked:
                            opticsSection.inspector.editor
                                .clearManualOpticsProfile()
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: {
                const receipt =
                    opticsSection.inspector.editor.opticsReceipt
                if (!opticsSection.inspector.editor.opticsEnabled)
                    return qsTranslate(
                        "PrecisionWorkspace",
                        "No profile correction is applied.")
                if (receipt.status === "matched")
                    return qsTranslate(
                        "PrecisionWorkspace",
                        "All calibrated corrections from this profile are applied together.")
                return qsTranslate(
                    "PrecisionWorkspace",
                    "No matching profile correction is available for this photo.")
            }
            color: opticsSection.inspector.editor.opticsEnabled
                && opticsSection.inspector.editor.opticsReceipt.status
                    === "matched"
                ? Theme.textSecondary : Theme.textMuted
            font.pixelSize: 9
            wrapMode: Text.WordWrap
            ToolTip.visible: opticsProfileSummaryHover.hovered
            ToolTip.delay: 500
            ToolTip.text: qsTranslate(
                "PrecisionWorkspace",
                "A profile is one coherent correction. Distortion, lateral chromatic aberration, lens shading, and automatic crop use every calibrated record that Lensfun provides; residual adjustments live in Manual.")
            HoverHandler { id: opticsProfileSummaryHover }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? implicitHeight : 0
        Layout.topMargin: 4
        visible: opticsSection.currentTabIndex === 1
        spacing: 0

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTranslate("PrecisionWorkspace", "Distortion")
            toolTipText: qsTranslate(
                "PrecisionWorkspace",
                "Residual radial geometry correction applied after the selected profile.")
            from: -100
            to: 100
            neutralValue: 0
            stepSize: 1
            decimals: 0
            suffix: "%"
            value: opticsSection.inspector.editor.manualOpticsDistortion
            onEdited: value =>
                opticsSection.inspector.editor.manualOpticsDistortion =
                    Math.round(value)
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 5
            text: qsTranslate("PrecisionWorkspace", "CHROMATIC ABERRATION")
            toolTipText: qsTranslate(
                "PrecisionWorkspace",
                "Geometrically realign color channels. This is distinct from purple/green Defringe below.")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTranslate("PrecisionWorkspace", "Red / Cyan")
            toolTipText: qsTranslate(
                "PrecisionWorkspace",
                "Move the red channel radially against green to correct red/cyan color fringes.")
            from: -100
            to: 100
            neutralValue: 0
            stepSize: 1
            decimals: 0
            suffix: "%"
            value: opticsSection.inspector.editor.manualOpticsTcaRedCyan
            onEdited: value =>
                opticsSection.inspector.editor.manualOpticsTcaRedCyan =
                    Math.round(value)
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTranslate("PrecisionWorkspace", "Blue / Yellow")
            toolTipText: qsTranslate(
                "PrecisionWorkspace",
                "Move the blue channel radially against green to correct blue/yellow color fringes.")
            from: -100
            to: 100
            neutralValue: 0
            stepSize: 1
            decimals: 0
            suffix: "%"
            value: opticsSection.inspector.editor.manualOpticsTcaBlueYellow
            onEdited: value =>
                opticsSection.inspector.editor.manualOpticsTcaBlueYellow =
                    Math.round(value)
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 5
            text: qsTranslate("PrecisionWorkspace", "VIGNETTING")
            toolTipText: qsTranslate(
                "PrecisionWorkspace",
                "Lens shading correction in original optical coordinates. The creative post-crop vignette is in Looks.")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTranslate("PrecisionWorkspace", "Amount")
            toolTipText: qsTranslate(
                "PrecisionWorkspace",
                "Brighten or darken the outer lens shading before crop and creative grading.")
            from: -100
            to: 100
            neutralValue: 0
            stepSize: 1
            decimals: 0
            suffix: "%"
            value: opticsSection.inspector.editor.manualOpticsVignettingAmount
            onEdited: value =>
                opticsSection.inspector.editor.manualOpticsVignettingAmount =
                    Math.round(value)
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTranslate("PrecisionWorkspace", "Midpoint")
            from: 0
            to: 100
            neutralValue: 50
            stepSize: 1
            decimals: 0
            suffix: "%"
            value: opticsSection.inspector.editor.manualOpticsVignettingMidpoint
            onEdited: value =>
                opticsSection.inspector.editor.manualOpticsVignettingMidpoint =
                    Math.round(value)
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 4
            text: qsTranslate("PrecisionWorkspace", "DEFRINGE")
            toolTipText: qsTranslate(
                "PrecisionWorkspace",
                "Suppress purple and green chromatic fringes within the selected hue ranges.")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTranslate("PrecisionWorkspace", "Purple amount")
            from: 0
            to: 1
            neutralValue: 0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value:
                opticsSection.inspector.fineValue("defringe_purple_amount")
            onGestureStarted:
                opticsSection.inspector.editor.beginParameterEdit(
                    "defringe_purple_amount")
            onEdited: value =>
                opticsSection.inspector.editor.setParameterValue(
                    "defringe_purple_amount", value)
            onGestureFinished:
                opticsSection.inspector.editor.endParameterEdit(
                    "defringe_purple_amount")
        }

        ShadowHueRange {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTranslate("PrecisionWorkspace", "Purple hue")
            accent: "#b66bd3"
            lowerValue:
                opticsSection.inspector.fineValue("defringe_purple_hue_low")
            upperValue:
                opticsSection.inspector.fineValue("defringe_purple_hue_high")
            onGestureStarted:
                opticsSection.inspector.editor.beginParameterEdit(
                    "optics/defringe/purple/hue_range")
            onEdited: (lowerValue, upperValue) =>
                opticsSection.inspector.editor.setDefringeHueRange(
                    "purple", lowerValue, upperValue)
            onGestureFinished:
                opticsSection.inspector.editor.endParameterEdit(
                    "optics/defringe/purple/hue_range")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTranslate("PrecisionWorkspace", "Green amount")
            from: 0
            to: 1
            neutralValue: 0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: opticsSection.inspector.fineValue("defringe_green_amount")
            onGestureStarted:
                opticsSection.inspector.editor.beginParameterEdit(
                    "defringe_green_amount")
            onEdited: value =>
                opticsSection.inspector.editor.setParameterValue(
                    "defringe_green_amount", value)
            onGestureFinished:
                opticsSection.inspector.editor.endParameterEdit(
                    "defringe_green_amount")
        }

        ShadowHueRange {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTranslate("PrecisionWorkspace", "Green hue")
            accent: "#58a66b"
            lowerValue:
                opticsSection.inspector.fineValue("defringe_green_hue_low")
            upperValue:
                opticsSection.inspector.fineValue("defringe_green_hue_high")
            onGestureStarted:
                opticsSection.inspector.editor.beginParameterEdit(
                    "optics/defringe/green/hue_range")
            onEdited: (lowerValue, upperValue) =>
                opticsSection.inspector.editor.setDefringeHueRange(
                    "green", lowerValue, upperValue)
            onGestureFinished:
                opticsSection.inspector.editor.endParameterEdit(
                    "optics/defringe/green/hue_range")
        }
    }
}
