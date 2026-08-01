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

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6

            CheckBox {
                id: enabledCheckBox
                objectName: "rawAiDenoiseEnabledCheckBox"
                Layout.fillWidth: true
                implicitHeight: 28
                checked: denoise.editor.foundationAiDenoiseEnabled
                enabled: !denoise.editor.foundationAiDenoiseBusy
                    && (checked || denoise.editor.foundationAiDenoiseCanStart)
                text: qsTr("Enable AI RAW Denoise")
                Accessible.name: text
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: checked
                    ? qsTr("Turn off AI RAW Denoise")
                    : qsTr("Run once, then keep a reversible cached foundation")
                onClicked:
                    denoise.editor.foundationAiDenoiseEnabled =
                        !denoise.editor.foundationAiDenoiseEnabled

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
        }
    }
}
