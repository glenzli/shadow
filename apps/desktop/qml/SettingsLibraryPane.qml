pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root

    required property var preferences
    signal openLutLibraryRequested()
    signal openOpticsProfileLibraryRequested()

    contentWidth: availableWidth
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var metadataFields: [
        { key: "captured_at", title: qsTr("Capture time") },
        { key: "location", title: qsTr("Location") },
        { key: "camera", title: qsTr("Camera") },
        { key: "lens", title: qsTr("Lens") },
        { key: "exposure", title: qsTr("Shutter speed") },
        { key: "aperture", title: qsTr("Aperture") },
        { key: "iso", title: qsTr("ISO") },
        { key: "focal_length", title: qsTr("Focal length") },
        { key: "dimensions", title: qsTr("Preview dimensions") },
        { key: "focal_length_35mm", title: qsTr("35 mm equivalent") },
        { key: "raw_dimensions", title: qsTr("RAW dimensions") },
        { key: "sensor_bits", title: qsTr("Sensor bit depth") },
        { key: "cfa", title: qsTr("Color filter array") },
        { key: "dng", title: qsTr("DNG version") }
    ]

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
            text: qsTr("Library")
            color: Theme.textPrimary
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Control Library density, metadata presentation, and reusable photographic resources.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        SettingsCard {
            Label {
                text: qsTr("Thumbnail size")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            ShadowSlider {
                objectName: "settingsThumbnailScaleSlider"
                Layout.fillWidth: true
                label: qsTr("Size")
                from: 96
                to: 360
                stepSize: 4
                neutralValue: 188
                fillFromMinimum: true
                decimals: 0
                suffix: " px"
                value: root.preferences.libraryThumbnailScale
                onEdited: value =>
                    root.preferences.libraryThumbnailScale = Math.round(value)
            }
        }

        SettingsCard {
            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Metadata fields")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                }

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Restore Defaults")
                    onClicked: root.preferences.resetExifFields()
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 12
                rowSpacing: 2

                Repeater {
                    model: root.metadataFields

                    ShadowCheckBox {
                        required property var modelData
                        Layout.fillWidth: true
                        text: modelData.title
                        checked: root.preferences.exifFields.indexOf(modelData.key) >= 0
                        onToggled:
                            root.preferences.setExifFieldVisible(modelData.key, checked)
                    }
                }
            }
        }

        SettingsCard {
            Label {
                text: qsTr("Photographic resources")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Manage reusable looks and optical profiles in their dedicated libraries.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ShadowButton {
                    text: qsTr("LUT Library…")
                    onClicked: root.openLutLibraryRequested()
                }

                ShadowButton {
                    text: qsTr("Optics Profiles…")
                    onClicked: root.openOpticsProfileLibraryRequested()
                }

                Item { Layout.fillWidth: true }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
