pragma ComponentBehavior: Bound
pragma Translator: "ExportDialog"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the editable export option draft and its exact backend projection.
// Preset transactions and the surrounding export lifecycle remain separate.
Item {
    id: settings

    required property var exportController
    property string format: "jpeg"
    property int maxEdge: 0
    property int quality: 90
    property int tiffBitDepth: 16
    property string colorSpace: "srgb"
    property int resolutionDpi: 300
    property string metadataPolicy: "none"
    property string creator: ""
    property string copyrightNotice: ""
    property string filenameSuffix: ""

    function resetFromFirstPreset() {
        const presets = exportController.presets || []
        applyPreset(presets.length > 0 ? presets[0] : null)
    }

    function applyPreset(preset) {
        if (!preset)
            return
        format = String(preset.format || "jpeg")
        tiffBitDepth = format === "tiff" ? Number(preset.bitDepth || 8) : 16
        maxEdge = Number(preset.maxEdge || 0)
        quality = Number(preset.quality || 90)
        colorSpace = String(preset.colorSpace || "srgb")
        resolutionDpi = Number(preset.resolutionDpi || 300)
        metadataPolicy = String(preset.metadataPolicy || "none")
        creator = String(preset.creator || "")
        copyrightNotice = String(preset.copyrightNotice || "")
        filenameSuffix = String(preset.filenameSuffix || "")
        watermarkPane.applySnapshot(preset)
        sizeField.text = maxEdge > 0 ? String(maxEdge) : ""
        suffixField.text = filenameSuffix
        resolutionField.text = String(resolutionDpi)
        creatorField.text = creator
        copyrightField.text = copyrightNotice
    }

    function options() {
        const parsedEdge = Number(sizeField.text)
        const parsedDpi = Number(resolutionField.text)
        const output = {
            "format": format,
            "maxEdge": Number.isFinite(parsedEdge) ? Math.max(0, Math.min(16384, Math.round(parsedEdge))) : 0,
            "quality": Math.round(quality),
            "bitDepth": format === "tiff" ? Math.round(tiffBitDepth) : 8,
            "colorSpace": String(colorSpace),
            "resolutionDpi": Number.isFinite(parsedDpi) ? Math.max(1, Math.min(2400, Math.round(parsedDpi))) : 300,
            "metadataPolicy": String(metadataPolicy),
            "creator": String(creatorField.text),
            "copyrightNotice": String(copyrightField.text),
            "filenameSuffix": String(suffixField.text)
        }
        const watermark = watermarkPane.snapshot()
        for (const key in watermark)
            output[key] = watermark[key]
        return output
    }

    ScrollView {
        anchors.fill: parent
        clip: true
        enabled: !settings.exportController.busy

    Item {
        width: parent.width
        implicitHeight: exportOptions.implicitHeight + 32

        ColumnLayout {
            id: exportOptions
            x: 20
            y: 16
            width: Math.max(0, parent.width - 40)
            spacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    text: qsTr("FILE")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.7
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    ShadowTabButton {
                        Layout.fillWidth: true
                        text: "JPEG"
                        active: settings.format === "jpeg"
                        onClicked: settings.format = "jpeg"
                    }
                    ShadowTabButton {
                        Layout.fillWidth: true
                        text: "PNG"
                        active: settings.format === "png"
                        onClicked: settings.format = "png"
                    }
                    ShadowTabButton {
                        Layout.fillWidth: true
                        text: "TIFF"
                        active: settings.format === "tiff"
                        onClicked: settings.format = "tiff"
                    }
                    ShadowTabButton {
                        Layout.fillWidth: true
                        text: "DNG"
                        active: settings.format === "dng"
                        onClicked: settings.format = "dng"
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: dngExplanation.implicitHeight + 16
                    visible: settings.format === "dng"
                    color: Theme.control
                    radius: Theme.compactControlRadius
                    border.width: 1
                    border.color: Theme.border
                    Label {
                        id: dngExplanation
                        anchors.fill: parent
                        anchors.margins: 8
                        text: qsTr("RAW DNG preserves the original sensor mosaic and source calibration. Edits, resizing, color space, metadata, and watermarks are not applied.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: settings.format !== "dng"
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Long edge")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                    ShadowTextField {
                        id: sizeField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Original size")
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator {
                            bottom: 0
                            top: 16384
                        }
                        color: Theme.textPrimary
                        placeholderTextColor: Theme.textPlaceholder
                        selectByMouse: true
                    }
                    Label {
                        text: "px"
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: settings.format === "tiff"
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Bit depth")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                    ShadowTabButton {
                        Layout.fillWidth: true
                        text: "8-bit"
                        active: settings.tiffBitDepth === 8
                        onClicked: settings.tiffBitDepth = 8
                    }
                    ShadowTabButton {
                        Layout.fillWidth: true
                        text: "16-bit"
                        active: settings.tiffBitDepth === 16
                        onClicked: settings.tiffBitDepth = 16
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: settings.format !== "dng"
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Color space")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                    ShadowComboBox {
                        id: colorSpaceBox
                        Layout.fillWidth: true
                        implicitHeight: Theme.controlHeight
                        model: ["sRGB", "Display P3"]
                        currentIndex: settings.colorSpace === "display-p3" ? 1 : 0
                        onActivated: settings.colorSpace = currentIndex === 1 ? "display-p3" : "srgb"
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: settings.format !== "dng"
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Resolution")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                    ShadowTextField {
                        id: resolutionField
                        Layout.fillWidth: true
                        text: "300"
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator {
                            bottom: 1
                            top: 2400
                        }
                        color: Theme.textPrimary
                        selectByMouse: true
                    }
                    Label {
                        text: "DPI"
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: settings.format === "jpeg"
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Quality")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                    ShadowInlineSlider {
                        Layout.fillWidth: true
                        from: 1
                        to: 100
                        neutralValue: 90
                        fillFromMinimum: true
                        stepSize: 1
                        value: settings.quality
                        onMoved: settings.quality = Math.round(value)
                        onResetRequested: value =>
                            settings.quality = Math.round(value)
                    }
                    Label {
                        Layout.preferredWidth: 30
                        text: String(settings.quality)
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        horizontalAlignment: Text.AlignRight
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Filename suffix")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                    ShadowTextField {
                        id: suffixField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Optional, e.g. _web")
                        color: Theme.textPrimary
                        placeholderTextColor: Theme.textPlaceholder
                        selectByMouse: true
                        onTextChanged: settings.filenameSuffix = text
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                visible: settings.format !== "dng"
                color: Theme.border
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: settings.format !== "dng"
                spacing: 9
                Label {
                    text: qsTr("METADATA")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.7
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Include")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                    ShadowComboBox {
                        id: metadataBox
                        Layout.fillWidth: true
                        implicitHeight: Theme.controlHeight
                        model: [qsTr("No metadata"), qsTr("Copyright only")]
                        currentIndex: settings.metadataPolicy === "copyright-only" ? 1 : 0
                        onActivated: settings.metadataPolicy = currentIndex === 1 ? "copyright-only" : "none"
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: settings.metadataPolicy === "copyright-only"
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Creator")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                    ShadowTextField {
                        id: creatorField
                        Layout.fillWidth: true
                        color: Theme.textPrimary
                        placeholderText: qsTr("Optional")
                        placeholderTextColor: Theme.textPlaceholder
                        selectByMouse: true
                        onTextChanged: settings.creator = text
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: settings.metadataPolicy === "copyright-only"
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Copyright")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                    }
                    ShadowTextField {
                        id: copyrightField
                        Layout.fillWidth: true
                        color: Theme.textPrimary
                        placeholderText: qsTr("Optional")
                        placeholderTextColor: Theme.textPlaceholder
                        selectByMouse: true
                        onTextChanged: settings.copyrightNotice = text
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                visible: settings.format !== "dng"
                color: Theme.border
            }

            ExportWatermarkPane {
                id: watermarkPane
                Layout.fillWidth: true
                visible: settings.format !== "dng"
                exportController: settings.exportController
            }
        }
    }
    }
}
