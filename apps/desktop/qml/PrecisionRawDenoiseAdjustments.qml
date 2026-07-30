pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: denoise

    required property var editor
    required property color textPrimary
    required property color textMuted
    required property color accent

    spacing: 8

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        title: qsTr("AI RAW DENOISE")
        summary: denoise.editor.foundationAiDenoiseEnabled
            ? qsTr("%L1%").arg(denoise.editor.foundationAiDenoiseAmount)
            : qsTr("OFF")
        expanded: true
        toolTipText: qsTr("A fixed photo-local node before Basic Adjustments.")
        resetAvailable: true
        resetEnabled: denoise.editor.foundationAiDenoiseEnabled
            && !denoise.editor.foundationAiDenoiseBusy
        resetObjectName: "rawAiDenoiseBypassButton"
        resetToolTipText: qsTr("Turn off AI RAW Denoise")
        onResetRequested:
            denoise.editor.foundationAiDenoiseEnabled = false

        ShadowSlider {
            objectName: "rawAiDenoiseAmountSlider"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Strength")
            from: 0
            to: 100
            neutralValue: 100
            stepSize: 1
            decimals: 0
            suffix: "%"
            value: denoise.editor.foundationAiDenoiseAmount
            enabled: denoise.editor.foundationAiDenoiseEnabled
                && !denoise.editor.foundationAiDenoiseBusy
            onGestureStarted:
                denoise.editor.beginParameterEdit("raw_ai_denoise/amount")
            onEdited: value =>
                denoise.editor.foundationAiDenoiseAmount = Math.round(value)
            onGestureFinished:
                denoise.editor.endParameterEdit("raw_ai_denoise/amount")
        }

        ProgressBar {
            objectName: "rawAiDenoiseProgress"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: denoise.editor.foundationAiDenoiseBusy
            from: 0
            to: 1
            value: denoise.editor.foundationAiDenoiseProgress
            indeterminate:
                denoise.editor.foundationAiDenoisePhase === "checking"
                || denoise.editor.foundationAiDenoisePhase === "queued"
                || denoise.editor.foundationAiDenoisePhase === "planning"
                || value <= 0
        }

        Label {
            objectName: "rawAiDenoiseStatus"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: denoise.editor.foundationAiDenoiseBusy
                || denoise.editor.foundationAiDenoisePhase === "failed"
                || denoise.editor.foundationAiDenoisePhase === "unavailable"
            text: denoise.editor.foundationAiDenoiseStatusText
            color:
                denoise.editor.foundationAiDenoisePhase === "failed"
                || denoise.editor.foundationAiDenoisePhase === "unavailable"
                    ? Theme.dangerText : denoise.textMuted
            font.pixelSize: 9
            elide: Text.ElideRight
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6

            ShadowButton {
                objectName: "rawAiDenoiseStartButton"
                Layout.fillWidth: true
                visible: !denoise.editor.foundationAiDenoiseEnabled
                    && !denoise.editor.foundationAiDenoiseBusy
                enabled: denoise.editor.foundationAiDenoiseCanStart
                compact: true
                variant: ShadowButton.Tinted
                text:
                    denoise.editor.foundationAiDenoisePhase === "failed"
                    || denoise.editor.foundationAiDenoisePhase === "unavailable"
                    || denoise.editor.foundationAiDenoisePhase === "cancelled"
                        ? qsTr("Retry AI Denoise")
                        : qsTr("Apply AI Denoise")
                onClicked: denoise.editor.startFoundationAiDenoise()
            }

            ShadowButton {
                objectName: "rawAiDenoiseCancelButton"
                Layout.fillWidth: true
                visible: denoise.editor.foundationAiDenoiseBusy
                enabled: denoise.editor.foundationAiDenoiseCanCancel
                compact: true
                variant: ShadowButton.Danger
                text: qsTr("Cancel")
                onClicked: denoise.editor.cancelFoundationAiDenoise()
            }
        }

    }
}
