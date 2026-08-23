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

    function fineValue(key) {
        const revision = foundation.editor.parameterRevision
        return revision >= 0 ? foundation.editor.parameterValue(key) : 0
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        title: qsTr("RAW WHITE BALANCE")
        summary: foundation.editor.foundationWhiteBalanceAtCameraValue
            && !foundation.editor.foundationWhiteBalanceCameraValueAvailable
            ? qsTr("Camera white balance · calibrated Kelvin unavailable")
            : qsTr("%L1 K · Tint %L2")
                .arg(foundation.editor.foundationWhiteBalanceTemperature)
                .arg(foundation.editor.foundationWhiteBalanceTint)
        expanded: true
        toolTipText: qsTr("Absolute photographic temperature and tint for this RAW. Camera-space calibration remains an internal rendering detail.")
        resetAvailable: true
        resetEnabled: !foundation.editor.foundationWhiteBalanceAtCameraValue
            && foundation.editor.active
            && !foundation.editor.stateBusy
        resetObjectName: "foundationWhiteBalanceResetButton"
        resetToolTipText: qsTr("Restore the white balance recorded by the camera")
        onResetRequested: foundation.editor.resetFoundationWhiteBalance()

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6
            Item { Layout.fillWidth: true }
            ShadowIconButton {
                source: "qrc:/icons/eyedropper.svg"
                selected: foundation.editor.rawWhiteBalancePickerActive
                enabled: foundation.editor.active && !foundation.editor.stateBusy
                toolTipText: qsTr("Pick a neutral area for RAW White Balance")
                accessibleName: toolTipText
                onClicked: foundation.editor.setRawWhiteBalancePickerActive(
                    !foundation.editor.rawWhiteBalancePickerActive)
            }
        }

        ShadowSlider {
            objectName: "foundationWhiteBalanceTemperatureSlider"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Temperature")
            toolTipText: qsTr("Set the absolute RAW white point as a correlated color temperature.")
            from: 2000
            to: 25000
            neutralValue: 5500
            stepSize: 50
            decimals: 0
            suffix: " K"
            value: foundation.editor.foundationWhiteBalanceTemperature
            semanticTrack: true
            trackStartColor: "#5f8bd8"
            trackMiddleColor: Theme.track
            trackEndColor: "#d4ad38"
            onGestureStarted: foundation.editor.beginParameterEdit(
                "foundation/raw_white_balance/temperature")
            onEdited: value =>
                foundation.editor.foundationWhiteBalanceTemperature =
                    Math.round(value)
            onGestureFinished: foundation.editor.endParameterEdit(
                "foundation/raw_white_balance/temperature")
        }

        ShadowSlider {
            objectName: "foundationWhiteBalanceTintSlider"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Tint")
            toolTipText: qsTr("Move the absolute RAW white point from green toward magenta.")
            from: -150
            to: 150
            neutralValue: 0
            stepSize: 1
            decimals: 0
            value: foundation.editor.foundationWhiteBalanceTint
            semanticTrack: true
            trackStartColor: "#55a36d"
            trackMiddleColor: Theme.track
            trackEndColor: "#c35a9d"
            onGestureStarted: foundation.editor.beginParameterEdit(
                "foundation/raw_white_balance/tint")
            onEdited: value =>
                foundation.editor.foundationWhiteBalanceTint =
                    Math.round(value)
            onGestureFinished: foundation.editor.endParameterEdit(
                "foundation/raw_white_balance/tint")
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        title: qsTr("RAW HIGHLIGHT REPAIR")
        summary: foundation.editor.foundationHighlightRepairEnabled
            ? qsTr("Soft transition")
            : qsTr("Off")
        toolTipText: qsTr("Neutralizes unreliable clipped RAW colour across a soft local transition. It does not reconstruct missing detail or colour.")
        resetAvailable: foundation.editor.foundationHighlightRepairEnabled
        resetEnabled: foundation.editor.active && !foundation.editor.stateBusy
        resetObjectName: "foundationHighlightRepairResetButton"
        onResetRequested: foundation.editor.foundationHighlightRepairEnabled = false

        ShadowSwitch {
            objectName: "foundationHighlightRepairSwitch"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            text: qsTr("Soft highlight transition")
            checked: foundation.editor.foundationHighlightRepairEnabled
            enabled: foundation.editor.active && !foundation.editor.stateBusy
            onToggled: foundation.editor.foundationHighlightRepairEnabled = checked
        }
    }

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: !foundation.editor.foundationSelected
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("GRADE WHITE BALANCE")
        toolTipText: qsTr("Relative processed-RGB temperature and tint for the selected Grade Node.")
        resetAvailable: true
        onResetRequested:
            foundation.editor.resetSelectedAdjustmentSection("white_balance")

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
        visible: !foundation.editor.foundationSelected
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("LIGHT")
        expanded: true
        resetAvailable: true
        onResetRequested:
            foundation.editor.resetSelectedAdjustmentSection("light")

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
        visible: !foundation.editor.foundationSelected
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("PRESENCE")
        toolTipText: qsTr("Foundational atmosphere and frequency controls evaluated before creative color grading.")
        resetAvailable: true
        onResetRequested:
            foundation.editor.resetSelectedAdjustmentSection("presence")

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
        visible: !foundation.editor.foundationSelected
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("COLOR")
        expanded: true
        resetAvailable: true
        onResetRequested:
            foundation.editor.resetSelectedAdjustmentSection("color")

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
        visible: !foundation.editor.foundationSelected
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("COLOR BALANCE")
        toolTipText: qsTr("Perceptual global opponent balance after basic color and before hue-keyed color corrections.")
        resetAvailable: true
        onResetRequested:
            foundation.editor.resetSelectedAdjustmentSection("color_balance")

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
        visible: !foundation.editor.foundationSelected
        enabled: foundation.gradeControlsEnabled
        opacity: enabled ? 1.0 : 0.42
        title: qsTr("CURVE")
        toolTipText: qsTr("Perceptual lightness curve; hue and chroma are preserved.")
        resetAvailable: true
        onResetRequested: foundation.editor.resetToneCurve()

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
