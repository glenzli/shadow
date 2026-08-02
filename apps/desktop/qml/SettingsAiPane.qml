pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root

    required property var aiPreferences
    required property var editor
    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    component SettingsCard: Rectangle {
        Layout.fillWidth: true
        implicitHeight: cardContent.implicitHeight + 28
        radius: Theme.controlRadius + 2
        color: Theme.panel
        border.width: 1
        border.color: Theme.border
        default property alias content: cardContent.data

        ColumnLayout {
            id: cardContent
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 14

        Label {
            Layout.fillWidth: true
            text: qsTr("AI & Models")
            color: Theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Shadow runs supported AI models locally. These permissions control new model work; disabling one does not remove or invalidate a result already generated for a photo.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        SettingsCard {
            ShadowSwitch {
                objectName: "rawDenoiseExecutionPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Allow AI RAW Denoise processing")
                checked: root.aiPreferences.rawDenoiseExecutionAllowed
                onToggled: root.aiPreferences.rawDenoiseExecutionAllowed = checked
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("The node remains non-destructive. A cached full-strength foundation can still be shown, hidden, or blended when new execution is disabled.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            ShadowSlider {
                objectName: "rawDenoiseDefaultAmountSlider"
                Layout.fillWidth: true
                label: qsTr("Default strength")
                from: 0
                to: 100
                stepSize: 1
                neutralValue: 100
                fillFromMinimum: true
                decimals: 0
                suffix: "%"
                value: root.aiPreferences.rawDenoiseDefaultAmount
                onEdited: value =>
                    root.aiPreferences.rawDenoiseDefaultAmount = Math.round(value)
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("This applies only when a new AI RAW Denoise node is added. Existing photos keep their authored strength.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: root.editor.active && root.editor.rawDenoiseNodeMaterialized
                text: root.editor.foundationAiDenoiseAvailable
                    ? qsTr("Current photo: local RAW denoise model is ready.")
                    : root.editor.foundationAiDenoiseStatusText
                color: root.editor.foundationAiDenoiseAvailable
                    ? Theme.successText : Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }
        }

        SettingsCard {
            ShadowSwitch {
                objectName: "subjectMaskExecutionPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Allow AI subject selection")
                checked: root.aiPreferences.subjectMaskExecutionAllowed
                onToggled: root.aiPreferences.subjectMaskExecutionAllowed = checked
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Subject selection uses the local SAM 2.1 provider when installed. Photo pixels and prompts are not uploaded by this feature.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }
        }

        SettingsCard {
            Label {
                text: qsTr("Model storage")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: root.aiPreferences.modelStoragePath
                color: Theme.textMuted
                font.family: "Menlo"
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideMiddle
            }

            RowLayout {
                Layout.fillWidth: true

                ShadowButton {
                    text: qsTr("Show Model Folder")
                    onClicked: Qt.openUrlExternally(root.aiPreferences.modelStorageUrl)
                }

                Item { Layout.fillWidth: true }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
