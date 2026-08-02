pragma ComponentBehavior: Bound
pragma Translator: "ExportDialog"

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Owns the editable export option draft and its exact backend projection.
// Preset transactions and the surrounding export lifecycle remain separate.
Item {
    id: settings

    required property var exportController
    property string format: "jpeg"
    property int maxEdge: 0
    property int quality: 90
    property string filenameSuffix: ""
    property string watermarkPath: ""
    property real watermarkOpacity: 0.72
    property real watermarkScale: 0.18
    property real watermarkInset: 0.02
    property string watermarkAnchor: "bottom-right"

    function resetFromFirstPreset() {
        const presets = exportController.presets || []
        presetBox.currentIndex = presets.length > 0 ? 0 : -1
        applyPreset(presetAt(presetBox.currentIndex))
    }

    function applyPreset(preset) {
        if (!preset)
            return
        format = String(preset.format || "jpeg")
        maxEdge = Number(preset.maxEdge || 0)
        quality = Number(preset.quality || 90)
        filenameSuffix = String(preset.filenameSuffix || "")
        watermarkPath = String(preset.watermarkPath || "")
        watermarkOpacity = Number(preset.watermarkOpacity !== undefined ? preset.watermarkOpacity : 0.72)
        watermarkScale = Number(preset.watermarkScale !== undefined ? preset.watermarkScale : 0.18)
        watermarkInset = Number(preset.watermarkInset !== undefined ? preset.watermarkInset : 0.02)
        watermarkAnchor = String(preset.watermarkAnchor || "bottom-right")
        sizeField.text = maxEdge > 0 ? String(maxEdge) : ""
        suffixField.text = filenameSuffix
    }

    function presetAt(index) {
        const presets = exportController.presets || []
        return index >= 0 && index < presets.length ? presets[index] : null
    }

    function selectedPreset() {
        return presetAt(presetBox.currentIndex)
    }

    function selectedPresetIsCustom() {
        const preset = selectedPreset()
        return preset !== null && !String(preset.id || "").startsWith("builtin-")
    }

    function selectPresetId(presetId) {
        const presets = exportController.presets || []
        for (let index = 0; index < presets.length; ++index) {
            if (String(presets[index].id || "") !== String(presetId))
                continue
            presetBox.currentIndex = index
            applyPreset(presets[index])
            return
        }
        presetBox.currentIndex = presets.length > 0 ? 0 : -1
        applyPreset(presetAt(presetBox.currentIndex))
    }

    function options() {
        const parsedEdge = Number(sizeField.text)
        return {
            "format": format,
            "maxEdge": Number.isFinite(parsedEdge) ? Math.max(0, Math.min(16384, Math.round(parsedEdge))) : 0,
            "quality": Math.round(quality),
            "filenameSuffix": String(suffixField.text),
            "watermarkPath": String(watermarkPath),
            "watermarkOpacity": Number(watermarkOpacity),
            "watermarkScale": Number(watermarkScale),
            "watermarkInset": Number(watermarkInset),
            "watermarkAnchor": String(watermarkAnchor)
        }
    }

    FileDialog {
        id: watermarkFileDialog
        title: qsTr("Choose a PNG watermark")
        nameFilters: [qsTr("PNG images (*.png)")]
        onAccepted: settings.watermarkPath = selectedFile.toString()
    }

    ExportPresetMenus {
        id: presetMenus
        exportController: settings.exportController
        onPresetSaved: presetId => settings.selectPresetId(presetId)
        onPresetRemoved: settings.selectPresetId("")
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
                spacing: 7
                Label {
                    text: qsTr("PRESET")
                    color: Theme.textMuted
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.7
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    ComboBox {
                        id: presetBox
                        Layout.fillWidth: true
                        model: settings.exportController.presets
                        textRole: "name"
                        valueRole: "id"
                        implicitHeight: Theme.controlHeight
                        onActivated: settings.applyPreset(settings.presetAt(currentIndex))
                        contentItem: Label {
                            leftPadding: 10
                            rightPadding: 28
                            verticalAlignment: Text.AlignVCenter
                            text: presetBox.displayText
                            color: Theme.textPrimary
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        background: Rectangle {
                            color: Theme.control
                            radius: Theme.compactControlRadius
                            border.width: 1
                            border.color: presetBox.activeFocus ? Theme.focusRing : Theme.border
                        }
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/node-add.svg"
                        toolTipText: qsTr("Save current settings as a preset")
                        accessibleName: toolTipText
                        onClicked: presetMenus.openSave(settings.options())
                    }

                    ShadowIconButton {
                        visible: settings.selectedPresetIsCustom()
                        source: "qrc:/icons/trash.svg"
                        toolTipText: qsTr("Remove selected export preset")
                        accessibleName: toolTipText
                        variant: ShadowIconButton.Ghost
                        onClicked: presetMenus.openRemove(settings.selectedPreset())
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    text: qsTr("FILE")
                    color: Theme.textMuted
                    font.pixelSize: 9
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
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Long edge")
                        color: Theme.textSecondary
                        font.pixelSize: 11
                    }
                    TextField {
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
                        background: Rectangle {
                            color: Theme.control
                            radius: Theme.compactControlRadius
                            border.width: 1
                            border.color: sizeField.activeFocus ? Theme.focusRing : Theme.border
                        }
                    }
                    Label {
                        text: "px"
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: settings.format === "jpeg"
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Quality")
                        color: Theme.textSecondary
                        font.pixelSize: 11
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
                        font.pixelSize: 10
                        horizontalAlignment: Text.AlignRight
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Filename suffix")
                        color: Theme.textSecondary
                        font.pixelSize: 11
                    }
                    TextField {
                        id: suffixField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Optional, e.g. _web")
                        color: Theme.textPrimary
                        placeholderTextColor: Theme.textPlaceholder
                        selectByMouse: true
                        onTextChanged: settings.filenameSuffix = text
                        background: Rectangle {
                            color: Theme.control
                            radius: Theme.compactControlRadius
                            border.width: 1
                            border.color: suffixField.activeFocus ? Theme.focusRing : Theme.border
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 9
                Label {
                    text: qsTr("PNG WATERMARK")
                    color: Theme.textMuted
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.7
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 7
                    ShadowButton {
                        Layout.fillWidth: true
                        text: settings.watermarkPath.length > 0 ? settings.watermarkPath.split("/").pop() : qsTr("CHOOSE PNG")
                        variant: ShadowButton.Secondary
                        onClicked: watermarkFileDialog.open()
                    }
                    ShadowIconButton {
                        source: "qrc:/icons/clear.svg"
                        toolTipText: qsTr("Remove watermark")
                        accessibleName: toolTipText
                        enabled: settings.watermarkPath.length > 0
                        onClicked: settings.watermarkPath = ""
                    }
                }
                GridLayout {
                    Layout.alignment: Qt.AlignHCenter
                    columns: 3
                    rowSpacing: 4
                    columnSpacing: 4
                    Repeater {
                        model: ["top-left", "top-center", "top-right", "middle-left", "middle-center", "middle-right", "bottom-left", "bottom-center", "bottom-right"]
                        delegate: Rectangle {
                            id: anchorCell
                            required property string modelData
                            width: 34
                            height: 26
                            radius: Theme.compactControlRadius
                            color: settings.watermarkAnchor === modelData ? Theme.accentSurface : Theme.controlQuiet
                            border.width: settings.watermarkAnchor === modelData ? 1 : 0
                            border.color: Theme.accentBorder
                            Rectangle {
                                anchors.centerIn: parent
                                width: 5
                                height: 5
                                radius: width / 2
                                color: settings.watermarkAnchor === anchorCell.modelData ? Theme.accent : Theme.textMuted
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: settings.watermarkAnchor = anchorCell.modelData
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Opacity")
                        color: Theme.textSecondary
                        font.pixelSize: 11
                    }
                    ShadowInlineSlider {
                        Layout.fillWidth: true
                        from: 0
                        to: 1
                        neutralValue: 0.72
                        fillFromMinimum: true
                        stepSize: 0.01
                        value: settings.watermarkOpacity
                        onMoved: settings.watermarkOpacity = value
                        onResetRequested: value =>
                            settings.watermarkOpacity = value
                    }
                    Label {
                        Layout.preferredWidth: 36
                        text: qsTr("%1%").arg(Math.round(settings.watermarkOpacity * 100))
                        color: Theme.textMuted
                        font.pixelSize: 10
                        horizontalAlignment: Text.AlignRight
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Scale")
                        color: Theme.textSecondary
                        font.pixelSize: 11
                    }
                    ShadowInlineSlider {
                        Layout.fillWidth: true
                        from: 0.03
                        to: 0.5
                        neutralValue: 0.18
                        fillFromMinimum: true
                        stepSize: 0.01
                        value: settings.watermarkScale
                        onMoved: settings.watermarkScale = value
                        onResetRequested: value =>
                            settings.watermarkScale = value
                    }
                    Label {
                        Layout.preferredWidth: 36
                        text: qsTr("%1%").arg(Math.round(settings.watermarkScale * 100))
                        color: Theme.textMuted
                        font.pixelSize: 10
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }
        }
    }
    }
}
