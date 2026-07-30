pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: foundation

    required property var editor
    required property bool gradeControlsEnabled
    required property color panelRaised
    required property color panelBorder
    required property color textPrimary
    required property color textMuted
    required property color accent

    spacing: 8

    function ratioStops(ratio) {
        return Math.log(Math.max(1.0 / 64.0, ratio)) / Math.LN2
    }

    function ratioText(ratio) {
        return Number(ratio).toLocaleString(Qt.locale(), "f", 3)
    }

    function fineValue(key) {
        const revision = foundation.editor.parameterRevision
        return revision >= 0 ? foundation.editor.parameterValue(key) : 0
    }

    function requestAiDenoiseEnabled(enabled) {
        foundation.editor.foundationAiDenoiseEnabled = enabled
    }

    function requestAiDenoiseStart() {
        foundation.editor.startFoundationAiDenoise()
    }

    function requestAiDenoiseCancel() {
        foundation.editor.cancelFoundationAiDenoise()
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        title: qsTr("AI RAW DENOISE")
        summary: foundation.editor.foundationAiDenoiseEnabled
            ? qsTr("ON") : qsTr("OFF")
        expanded: true
        toolTipText: qsTr("One photo-local RAW Foundation node. It is generated once, can be bypassed at any time, and does not replace conventional noise reduction.")

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Neural RAW Foundation")
                    color: foundation.textPrimary
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: foundation.editor.foundationAiDenoiseEnabled
                        ? qsTr("Scene-linear AI source for every later adjustment")
                        : qsTr("Build a verified reusable source from the original RAW")
                    color: foundation.textMuted
                    font.pixelSize: 9
                    elide: Text.ElideRight
                }
            }

            Switch {
                id: aiDenoiseSwitch

                objectName: "foundationAiDenoiseSwitch"
                Layout.preferredWidth: 36
                Layout.preferredHeight: 22
                checked: foundation.editor.foundationAiDenoiseEnabled
                enabled: foundation.editor.active
                    && !foundation.editor.stateBusy
                    && !foundation.editor.foundationAiDenoiseBusy
                    && (checked
                        || foundation.editor.foundationAiDenoiseCanStart)
                Accessible.name: checked
                    ? qsTr("Bypass AI RAW Denoise")
                    : qsTr("Enable AI RAW Denoise")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: checked
                    ? qsTr("Bypass the AI Foundation without deleting its cache")
                    : qsTr("Generate or reuse the verified AI Foundation")
                onClicked: foundation.requestAiDenoiseEnabled(checked)

                indicator: Rectangle {
                    implicitWidth: 34
                    implicitHeight: 18
                    x: (aiDenoiseSwitch.width - width) / 2
                    y: (aiDenoiseSwitch.height - height) / 2
                    radius: height / 2
                    color: aiDenoiseSwitch.checked
                        ? Theme.switchOnSurface : Theme.switchOffSurface
                    border.color: aiDenoiseSwitch.checked
                        ? Theme.switchOnBorder : Theme.switchOffBorder

                    Rectangle {
                        width: 12
                        height: 12
                        y: 3
                        x: aiDenoiseSwitch.checked
                            ? parent.width - width - 3 : 3
                        radius: width / 2
                        color: aiDenoiseSwitch.checked
                            ? foundation.accent : foundation.textMuted

                        Behavior on x {
                            NumberAnimation {
                                duration: 110
                                easing.type: Easing.OutCubic
                            }
                        }
                    }
                }

                contentItem: Item {}
            }
        }

        ProgressBar {
            id: aiDenoiseProgress

            objectName: "foundationAiDenoiseProgress"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 8
            visible: foundation.editor.foundationAiDenoiseBusy
            from: 0.0
            to: 1.0
            value: foundation.editor.foundationAiDenoiseProgress
            indeterminate:
                foundation.editor.foundationAiDenoisePhase === "checking"
                || foundation.editor.foundationAiDenoisePhase === "queued"
                || foundation.editor.foundationAiDenoisePhase === "planning"
                || value <= 0.0
        }

        Label {
            objectName: "foundationAiDenoiseStatus"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            text: foundation.editor.foundationAiDenoiseStatusText
            color:
                foundation.editor.foundationAiDenoisePhase === "failed"
                || foundation.editor.foundationAiDenoisePhase === "unavailable"
                    ? Theme.dangerText : foundation.textMuted
            font.pixelSize: 9
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 8
            spacing: 6

            ShadowButton {
                objectName: "foundationAiDenoiseStartButton"
                Layout.fillWidth: true
                visible: !foundation.editor.foundationAiDenoiseEnabled
                    && !foundation.editor.foundationAiDenoiseBusy
                enabled: foundation.editor.foundationAiDenoiseCanStart
                compact: true
                variant: ShadowButton.Tinted
                text:
                    foundation.editor.foundationAiDenoisePhase === "failed"
                    || foundation.editor.foundationAiDenoisePhase === "unavailable"
                    || foundation.editor.foundationAiDenoisePhase === "cancelled"
                        ? qsTr("Retry AI Denoise")
                        : qsTr("Build AI Foundation")
                toolTipText: qsTr("Materialize the verified RAW Foundation before enabling the saved switch.")
                onClicked: foundation.requestAiDenoiseStart()
            }

            ShadowButton {
                objectName: "foundationAiDenoiseCancelButton"
                Layout.fillWidth: true
                visible: foundation.editor.foundationAiDenoiseBusy
                enabled: foundation.editor.foundationAiDenoiseCanCancel
                compact: true
                variant: ShadowButton.Danger
                text: qsTr("Cancel")
                toolTipText: qsTr("Cancel the current materialization without changing the saved Recipe.")
                onClicked: foundation.requestAiDenoiseCancel()
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            text: qsTr("The learned-demosaiced scene-linear artifact is rebuildable cache, not a JPEG. Only the model identity and on/off intent are stored with the photo.")
            color: foundation.textMuted
            font.pixelSize: 9
            wrapMode: Text.Wrap
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        title: qsTr("RAW WHITE BALANCE")
        expanded: true
        toolTipText: qsTr("Absolute camera-space source interpretation for this photo. It is not part of the selected Grade Node.")

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6

            Label {
                Layout.preferredWidth: 68
                text: qsTr("Mode")
                color: foundation.textPrimary
                font.pixelSize: 10
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
            }

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                text: qsTr("As Shot")
                selected:
                    foundation.editor.foundationWhiteBalanceMode === 0
                toolTipText: qsTr("Use the white balance recorded by the camera.")
                onClicked:
                    foundation.editor.foundationWhiteBalanceMode = 0
            }

            ShadowButton {
                Layout.fillWidth: true
                compact: true
                text: qsTr("Custom")
                selected:
                    foundation.editor.foundationWhiteBalanceMode === 1
                toolTipText: qsTr("Use an exact camera-neutral ratio stored with this photo.")
                onClicked:
                    foundation.editor.foundationWhiteBalanceMode = 1
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 86
            Layout.rightMargin: 14
            visible: foundation.editor.foundationWhiteBalanceMode === 0
            text: qsTr("Uses the white balance recorded by the camera.")
            color: foundation.textMuted
            font.pixelSize: 9
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 86
            Layout.rightMargin: 14
            visible: foundation.editor.foundationWhiteBalanceMode === 1
            text: qsTr("Camera neutral · R %1 · G 1.000 · B %2")
                .arg(foundation.ratioText(
                    foundation.editor.foundationCameraNeutralRed))
                .arg(foundation.ratioText(
                    foundation.editor.foundationCameraNeutralBlue))
            color: foundation.textMuted
            font.pixelSize: 9
            wrapMode: Text.Wrap
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: foundation.editor.foundationWhiteBalanceMode === 1
            label: qsTr("Red / green")
            toolTipText: qsTr("Logarithmic red-to-green CameraNeutral ratio in the source camera space.")
            from: -6.0
            to: 6.0
            neutralValue: 0.0
            stepSize: 0.01
            decimals: 2
            suffix: " EV"
            value: foundation.ratioStops(
                foundation.editor.foundationCameraNeutralRed)
            semanticTrack: true
            trackStartColor: "#5f8bd8"
            trackMiddleColor: Theme.track
            trackEndColor: "#d85d66"
            onGestureStarted: foundation.editor.beginParameterEdit(
                "foundation/raw_white_balance/red")
            onEdited: value => {
                foundation.editor.foundationCameraNeutralRed =
                    Math.pow(2.0, value)
            }
            onGestureFinished: foundation.editor.endParameterEdit(
                "foundation/raw_white_balance/red")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: foundation.editor.foundationWhiteBalanceMode === 1
            label: qsTr("Blue / green")
            toolTipText: qsTr("Logarithmic blue-to-green CameraNeutral ratio in the source camera space.")
            from: -6.0
            to: 6.0
            neutralValue: 0.0
            stepSize: 0.01
            decimals: 2
            suffix: " EV"
            value: foundation.ratioStops(
                foundation.editor.foundationCameraNeutralBlue)
            semanticTrack: true
            trackStartColor: "#3979dc"
            trackMiddleColor: Theme.track
            trackEndColor: "#d4ad38"
            onGestureStarted: foundation.editor.beginParameterEdit(
                "foundation/raw_white_balance/blue")
            onEdited: value => {
                foundation.editor.foundationCameraNeutralBlue =
                    Math.pow(2.0, value)
            }
            onGestureFinished: foundation.editor.endParameterEdit(
                "foundation/raw_white_balance/blue")
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("GRADE WHITE BALANCE")
        toolTipText: qsTr("Relative processed-RGB temperature and tint for the selected Grade Node.")

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6
            Item { Layout.fillWidth: true }
            ShadowIconButton {
                source: "qrc:/icons/eyedropper.svg"
                selected: foundation.editor.whiteBalancePickerActive
                toolTipText: qsTr("Pick a neutral area for Grade White Balance")
                accessibleName: toolTipText
                onClicked: foundation.editor.setWhiteBalancePickerActive(
                    !foundation.editor.whiteBalancePickerActive)
            }
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Temperature")
            from: -1.0
            to: 1.0
            neutralValue: 0.0
            stepSize: 0.005
            decimals: 0
            displayMultiplier: 100
            value: foundation.editor.whiteBalanceTemperature
            semanticTrack: true
            trackStartColor: "#3979dc"
            trackMiddleColor: Theme.track
            trackEndColor: "#e49a3a"
            onGestureStarted: foundation.editor.beginParameterEdit(
                "white_balance_temperature")
            onEdited: value => foundation.editor.whiteBalanceTemperature = value
            onGestureFinished: foundation.editor.endParameterEdit(
                "white_balance_temperature")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Tint")
            from: -1.0
            to: 1.0
            neutralValue: 0.0
            stepSize: 0.005
            decimals: 0
            displayMultiplier: 100
            value: foundation.editor.whiteBalanceTint
            semanticTrack: true
            trackStartColor: "#48a56a"
            trackMiddleColor: Theme.track
            trackEndColor: "#c65ab4"
            onGestureStarted: foundation.editor.beginParameterEdit(
                "white_balance_tint")
            onEdited: value => foundation.editor.whiteBalanceTint = value
            onGestureFinished: foundation.editor.endParameterEdit(
                "white_balance_tint")
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("LIGHT")
        expanded: true

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Exposure")
            from: -5.0
            to: 5.0
            neutralValue: 0.0
            stepSize: 0.05
            value: foundation.editor.exposureStops
            suffix: " EV"
            onGestureStarted: foundation.editor.beginParameterEdit("exposure")
            onEdited: value => foundation.editor.exposureStops = value
            onGestureFinished: foundation.editor.endParameterEdit("exposure")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Contrast")
            from: 0.25
            to: 2.5
            neutralValue: 1.0
            stepSize: 0.01
            value: foundation.editor.contrastFactor
            suffix: "×"
            onGestureStarted: foundation.editor.beginParameterEdit("contrast")
            onEdited: value => foundation.editor.contrastFactor = value
            onGestureFinished: foundation.editor.endParameterEdit("contrast")
        }

        Repeater {
            model: [
                { "key": "highlights", "name": qsTr("Highlights") },
                { "key": "shadows", "name": qsTr("Shadows") },
                { "key": "whites", "name": qsTr("Whites") },
                { "key": "blacks", "name": qsTr("Blacks") }
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
                value: foundation.fineValue(modelData.key)
                onGestureStarted: foundation.editor.beginParameterEdit(modelData.key)
                onEdited: value => foundation.editor.setParameterValue(
                    modelData.key, value)
                onGestureFinished: foundation.editor.endParameterEdit(modelData.key)
            }
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("PRESENCE")
        toolTipText: qsTr("Foundational atmosphere and frequency controls evaluated before creative color grading.")

        Repeater {
            model: [
                {
                    "key": "dehaze",
                    "name": qsTr("Dehaze"),
                    "tip": qsTr("Restore atmospheric separation before creative grading.")
                },
                {
                    "key": "clarity",
                    "name": qsTr("Clarity"),
                    "tip": qsTr("Adjust protected mid-frequency structure without changing color.")
                },
                {
                    "key": "texture",
                    "name": qsTr("Texture"),
                    "tip": qsTr("Adjust fine lightness detail without sharpening edges or color noise.")
                },
                {
                    "key": "local_contrast",
                    // This is the photographic microcontrast control. The compact
                    // term fits the fixed slider-label column; its tooltip
                    // retains the full, distinct behavior.
                    "name": qsTr("Microcontrast"),
                    "tip": qsTr("Adjust broad edge-aware lightness contrast independently from Clarity and Texture.")
                }
            ]
            delegate: ShadowSlider {
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                label: modelData.name
                toolTipText: modelData.tip
                from: -1.0
                to: 1.0
                neutralValue: 0.0
                stepSize: 0.01
                decimals: 0
                displayMultiplier: 100
                suffix: "%"
                value: foundation.fineValue(modelData.key)
                onGestureStarted: foundation.editor.beginParameterEdit(modelData.key)
                onEdited: value => foundation.editor.setParameterValue(
                    modelData.key, value)
                onGestureFinished: foundation.editor.endParameterEdit(modelData.key)
            }
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            // The control immediately follows Microcontrast, so a short label
            // is unambiguous and avoids truncation in narrow panes.
            label: qsTr("Scale")
            toolTipText: qsTr("Choose the spatial scale used by Local Contrast, from medium to broad structure.")
            from: 0.0
            to: 1.0
            neutralValue: 0.5
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: foundation.fineValue("local_contrast_scale")
            onGestureStarted: foundation.editor.beginParameterEdit(
                "local_contrast_scale")
            onEdited: value => foundation.editor.setParameterValue(
                "local_contrast_scale", value)
            onGestureFinished: foundation.editor.endParameterEdit(
                "local_contrast_scale")
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("COLOR")
        expanded: true

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Chroma")
            from: 0.0
            to: 2.5
            neutralValue: 1.0
            stepSize: 0.01
            value: foundation.editor.saturationFactor
            suffix: "×"
            onGestureStarted: foundation.editor.beginParameterEdit("saturation")
            onEdited: value => foundation.editor.saturationFactor = value
            onGestureFinished: foundation.editor.endParameterEdit("saturation")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Vibrance")
            from: -1.0
            to: 1.0
            neutralValue: 0.0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: foundation.fineValue("vibrance")
            onGestureStarted: foundation.editor.beginParameterEdit("vibrance")
            onEdited: value => foundation.editor.setParameterValue("vibrance", value)
            onGestureFinished: foundation.editor.endParameterEdit("vibrance")
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("COLOR BALANCE")
        toolTipText: qsTr("Perceptual global opponent balance after basic color and before hue-keyed color corrections.")

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Green to red balance")
            startLabel: qsTr("Green")
            endLabel: qsTr("Red")
            from: -1.0
            to: 1.0
            neutralValue: 0.0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            semanticTrack: true
            trackStartColor: "#48a56a"
            trackMiddleColor: Theme.track
            trackEndColor: "#d85d66"
            value: foundation.fineValue("global_a_balance")
            onGestureStarted: foundation.editor.beginParameterEdit("global_a_balance")
            onEdited: value => foundation.editor.setParameterValue(
                "global_a_balance", value)
            onGestureFinished: foundation.editor.endParameterEdit("global_a_balance")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Blue to yellow balance")
            startLabel: qsTr("Blue")
            endLabel: qsTr("Yellow")
            from: -1.0
            to: 1.0
            neutralValue: 0.0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            semanticTrack: true
            trackStartColor: "#3979dc"
            trackMiddleColor: Theme.track
            trackEndColor: "#d4ad38"
            value: foundation.fineValue("global_b_balance")
            onGestureStarted: foundation.editor.beginParameterEdit("global_b_balance")
            onEdited: value => foundation.editor.setParameterValue(
                "global_b_balance", value)
            onGestureFinished: foundation.editor.endParameterEdit("global_b_balance")
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("CURVE")
        toolTipText: qsTr("Perceptual lightness curve; hue and chroma are preserved.")

        ToneCurveEditor {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.preferredHeight: implicitHeight
            controller: foundation.editor
            panelColor: foundation.panelRaised
            plotColor: Theme.chrome
            borderColor: foundation.panelBorder
            textColor: foundation.textPrimary
            mutedTextColor: foundation.textMuted
            accentColor: foundation.accent
        }
    }
}
