pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root

    required property var aiPreferences
    required property var imageUnderstandingController
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
            text: qsTr("Shadow uses locally managed Infer Runtime capabilities. These permissions control new model work; disabling one does not remove or invalidate a result already generated for a photo.")
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
                    ? qsTr("Current photo: Infer Runtime RAW Denoise is ready.")
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
                text: qsTr("Subject selection uses Infer Runtime when its SAM capability is available. Photo pixels and prompts remain local to this device.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }
        }

        SettingsCard {
            Label {
                text: qsTr("Photo understanding")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            ShadowSwitch {
                objectName: "imageUnderstandingExecutionPermissionSwitch"
                Layout.fillWidth: true
                text: qsTr("Allow local descriptions, keywords, and category review")
                checked: root.aiPreferences.imageUnderstandingExecutionAllowed
                onToggled:
                    root.aiPreferences.imageUnderstandingExecutionAllowed = checked
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("A heavier local vision model is used only for explicit review or eligible background photos. Its output remains a suggestion until the configured acceptance policy applies.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: understandingStatus.implicitHeight + 20
                radius: Theme.compactControlRadius
                color: Theme.surfaceSubtle
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    id: understandingStatus
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        BusyIndicator {
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 18
                            running: root.imageUnderstandingController.busy
                            visible: running
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.imageUnderstandingController.statusText
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            visible: root.imageUnderstandingController.totalPhotos > 0
                            text: root.imageUnderstandingController.progressPercent + "%"
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 3
                        radius: 2
                        color: Theme.border
                        visible: root.imageUnderstandingController.totalPhotos > 0

                        Rectangle {
                            width: parent.width
                                * root.imageUnderstandingController.progressPercent / 100
                            height: parent.height
                            radius: parent.radius
                            color: Theme.accent
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.imageUnderstandingController.errorText
                        color: Theme.dangerText
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Item { Layout.fillWidth: true }

                        ShadowButton {
                            compact: true
                            visible: root.imageUnderstandingController.canPause
                            text: qsTr("Pause")
                            onClicked: root.imageUnderstandingController.pause()
                        }

                        ShadowButton {
                            compact: true
                            visible: root.imageUnderstandingController.canResume
                            text: qsTr("Resume")
                            onClicked: root.imageUnderstandingController.resume()
                        }

                        ShadowButton {
                            compact: true
                            enabled: !root.imageUnderstandingController.busy
                                && root.aiPreferences.imageUnderstandingExecutionAllowed
                            text: qsTr("Scan again")
                            onClicked: root.imageUnderstandingController.refresh()
                        }
                    }
                }
            }

            ShadowSwitch {
                id: imageUnderstandingBackgroundSwitch
                objectName: "imageUnderstandingBackgroundSwitch"
                Layout.fillWidth: true
                text: qsTr("Analyze eligible photos in the background")
                enabled: root.aiPreferences.imageUnderstandingExecutionAllowed
                checked: root.aiPreferences.imageUnderstandingBackgroundEnabled
                onToggled:
                    root.aiPreferences.imageUnderstandingBackgroundEnabled = checked
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8
                visible: imageUnderstandingBackgroundSwitch.checked

                Label {
                    text: qsTr("Background range")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 6
                    rowSpacing: 6

                    Repeater {
                        model: [
                            { key: "liked", title: qsTr("Liked photos") },
                            { key: "minimum_rating", title: qsTr("By star rating") },
                            {
                                key: "liked_or_minimum_rating",
                                title: qsTr("Liked or highly rated")
                            },
                            { key: "all", title: qsTr("All photos") }
                        ]

                        ShadowButton {
                            required property var modelData
                            Layout.fillWidth: true
                            compact: true
                            selected:
                                root.aiPreferences.imageUnderstandingScanScope
                                === modelData.key
                            text: modelData.title
                            onClicked:
                                root.aiPreferences.imageUnderstandingScanScope = modelData.key
                        }
                    }
                }

                ShadowSlider {
                    objectName: "imageUnderstandingMinimumRatingSlider"
                    Layout.fillWidth: true
                    visible:
                        root.aiPreferences.imageUnderstandingScanScope
                            === "minimum_rating"
                        || root.aiPreferences.imageUnderstandingScanScope
                            === "liked_or_minimum_rating"
                    label: qsTr("Minimum rating")
                    from: 1
                    to: 5
                    stepSize: 1
                    neutralValue: 5
                    fillFromMinimum: true
                    decimals: 0
                    suffix: " ★"
                    value: root.aiPreferences.imageUnderstandingMinimumRating
                    onEdited: value =>
                        root.aiPreferences.imageUnderstandingMinimumRating = Math.round(value)
                }

                Label {
                    Layout.fillWidth: true
                    visible:
                        root.aiPreferences.imageUnderstandingScanScope === "all"
                    text: qsTr("A full Library can take a long time. Shadow processes it in small resumable batches and yields to interactive work.")
                    color: Theme.warningText
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }
            }

            ShadowSwitch {
                objectName: "imageUnderstandingAutoKeywordsSwitch"
                Layout.fillWidth: true
                text: qsTr("Automatically apply AI keyword suggestions")
                enabled: root.aiPreferences.imageUnderstandingExecutionAllowed
                checked: root.aiPreferences.imageUnderstandingAutoApplyKeywords
                onToggled:
                    root.aiPreferences.imageUnderstandingAutoApplyKeywords = checked
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("AI-owned keyword assignments may be refreshed when the model or image changes. Manually created and imported assignments are never removed.")
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
