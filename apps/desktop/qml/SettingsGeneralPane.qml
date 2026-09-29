pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root

    required property var preferences
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
            text: qsTr("General")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Choose how Shadow looks and which language it uses. Changes apply immediately.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        SettingsCard {
            Label {
                text: qsTr("Appearance")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: [
                        { key: "system", title: qsTr("Follow System") },
                        { key: "light", title: qsTr("Light") },
                        { key: "dark", title: qsTr("Dark") }
                    ]

                    ShadowButton {
                        required property var modelData
                        Layout.fillWidth: true
                        selected: root.preferences.appearanceMode === modelData.key
                        text: modelData.title
                        onClicked: root.preferences.appearanceMode = modelData.key
                    }
                }
            }
        }

        SettingsCard {
            Label {
                text: qsTr("Language")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: [
                        { key: "system", title: qsTr("Follow System") },
                        { key: "en", title: qsTr("English") },
                        { key: "zh_CN", title: qsTr("Simplified Chinese") }
                    ]

                    ShadowButton {
                        required property var modelData
                        Layout.fillWidth: true
                        selected: root.preferences.languageMode === modelData.key
                        text: modelData.title
                        onClicked: root.preferences.languageMode = modelData.key
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("The complete interface is retranslated without restarting Shadow.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }
        }

        Item { Layout.fillHeight: true }
    }
}
