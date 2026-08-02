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
            fillFromMinimum: true
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

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: denoise.editor.foundationAiDenoiseBusy
            spacing: 6

            ProgressBar {
                objectName: "rawAiDenoiseProgress"
                Layout.fillWidth: true
                from: 0
                to: 1
                value: denoise.editor.foundationAiDenoiseProgress
                indeterminate:
                    denoise.editor.foundationAiDenoisePhase === "checking"
                    || denoise.editor.foundationAiDenoisePhase === "queued"
                    || denoise.editor.foundationAiDenoisePhase === "planning"
                    || (denoise.editor.foundationAiDenoisePhase === "running"
                        && value <= 0.05)
                    || value <= 0
            }

            ShadowIconButton {
                objectName: "rawAiDenoiseCancelButton"
                enabled: denoise.editor.foundationAiDenoiseCanCancel
                source: "qrc:/icons/close.svg"
                variant: ShadowIconButton.Ghost
                foregroundColor: Theme.dangerText
                buttonSize: 22
                iconSize: 11
                toolTipText: qsTr("Cancel")
                onClicked: denoise.editor.cancelFoundationAiDenoise()
            }
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

        Label {
            objectName: "rawAiDenoiseNoiseRecommendation"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            text: denoise.editor.foundationAiDenoiseNoiseRecommendation
            color: denoise.editor.foundationAiDenoiseNoiseLevel === "high"
                ? Theme.warningText : denoise.textMuted
            font.pixelSize: 10
            elide: Text.ElideRight

            HoverHandler { id: noiseRecommendationHover }
            ToolTip.visible: noiseRecommendationHover.hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Source RAW noise estimate · %L1% confidence").arg(
                denoise.editor.foundationAiDenoiseNoiseConfidence)
        }

        CheckBox {
            id: enabledCheckBox
            objectName: "rawAiDenoiseEnableCheckBox"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            implicitHeight: 28
            checked: denoise.editor.foundationAiDenoiseRequested
            enabled: denoise.editor.foundationAiDenoiseCanStart
                || denoise.editor.foundationAiDenoiseCanApply
                || denoise.editor.foundationAiDenoiseCanCancel
                || denoise.editor.foundationAiDenoiseEnabled
            text: qsTr("Enable AI RAW Denoise")
            Accessible.name: text
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: denoise.editor.foundationAiDenoiseCanCancel
                ? qsTr("Cancel")
                : (denoise.editor.foundationAiDenoiseEnabled
                    ? qsTr("Turn off AI RAW Denoise")
                    : qsTr("Run once, then keep a reversible cached foundation"))
            onClicked: {
                if (!denoise.editor.foundationAiDenoiseRequested) {
                    denoise.editor.startFoundationAiDenoise()
                } else if (denoise.editor.foundationAiDenoiseCanCancel) {
                    denoise.editor.cancelFoundationAiDenoise()
                } else if (denoise.editor.foundationAiDenoiseEnabled) {
                    denoise.editor.foundationAiDenoiseEnabled = false
                }
            }

            indicator: Rectangle {
                implicitWidth: 16
                implicitHeight: 16
                x: 0
                y: Math.round((enabledCheckBox.height - height) / 2)
                radius: 4
                color: enabledCheckBox.checked
                    ? Theme.switchOnSurface : Theme.switchOffSurface
                border.width: 1
                border.color: enabledCheckBox.checked
                    ? Theme.switchOnBorder : Theme.borderStrong

                ShadowIcon {
                    anchors.centerIn: parent
                    visible: enabledCheckBox.checked
                    source: "qrc:/icons/check.svg"
                    color: denoise.accent
                    size: 11
                }
            }

            contentItem: Label {
                leftPadding: 24
                text: enabledCheckBox.text
                color: enabledCheckBox.enabled
                    ? denoise.textPrimary : denoise.textMuted
                font.pixelSize: 10
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }

        Label {
            objectName: "rawAiDenoiseNodeHiddenWarning"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: denoise.editor.foundationAiDenoiseRequested
                && !denoise.editor.rawDenoiseNodeVisible
            text: qsTr("Node hidden · AI result is not applied")
            color: Theme.warningText
            font.pixelSize: 9
            elide: Text.ElideRight
        }
    }
}
