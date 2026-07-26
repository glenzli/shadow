pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Popup {
    id: dialog

    required property var exportController
    property var targets: []
    property string format: "jpeg"
    property int maxEdge: 0
    property int quality: 90
    property string filenameSuffix: ""
    property string watermarkPath: ""
    property real watermarkOpacity: 0.72
    property real watermarkScale: 0.18
    property real watermarkInset: 0.02
    property string watermarkAnchor: "bottom-right"
    property var presetPendingRemoval: null

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(540, parent.width - 40)
    height: Math.min(720, parent.height - 40)
    padding: 0
    modal: true
    dim: true
    focus: true
    closePolicy: exportController.busy ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function present(exportTargets) {
        targets = exportTargets || [];
        if (exportController.presets.length > 0) {
            presetBox.currentIndex = 0;
            applyPreset(exportController.presets[0]);
        }
        open();
    }

    function applyPreset(preset) {
        if (!preset)
            return;
        format = String(preset.format || "jpeg");
        maxEdge = Number(preset.maxEdge || 0);
        quality = Number(preset.quality || 90);
        filenameSuffix = String(preset.filenameSuffix || "");
        watermarkPath = String(preset.watermarkPath || "");
        watermarkOpacity = Number(preset.watermarkOpacity !== undefined ? preset.watermarkOpacity : 0.72);
        watermarkScale = Number(preset.watermarkScale !== undefined ? preset.watermarkScale : 0.18);
        watermarkInset = Number(preset.watermarkInset !== undefined ? preset.watermarkInset : 0.02);
        watermarkAnchor = String(preset.watermarkAnchor || "bottom-right");
        sizeField.text = maxEdge > 0 ? String(maxEdge) : "";
        suffixField.text = filenameSuffix;
    }

    function presetAt(index) {
        const presets = exportController.presets || [];
        return index >= 0 && index < presets.length ? presets[index] : null;
    }

    function selectedPreset() {
        return presetAt(presetBox.currentIndex);
    }

    function selectedPresetIsCustom() {
        const preset = selectedPreset();
        return preset !== null && !String(preset.id || "").startsWith("builtin-");
    }

    function selectPresetId(presetId) {
        const presets = exportController.presets || [];
        for (let index = 0; index < presets.length; ++index) {
            if (String(presets[index].id || "") !== String(presetId))
                continue;
            presetBox.currentIndex = index;
            applyPreset(presets[index]);
            return;
        }
        presetBox.currentIndex = presets.length > 0 ? 0 : -1;
        applyPreset(presetAt(presetBox.currentIndex));
    }

    function requestPresetRemoval() {
        const preset = selectedPreset();
        if (preset === null || !selectedPresetIsCustom())
            return;
        presetPendingRemoval = preset;
        presetRemovalPopup.open();
    }

    function removePendingPreset() {
        const preset = presetPendingRemoval;
        presetRemovalPopup.close();
        presetPendingRemoval = null;
        if (preset === null || String(preset.id || "").startsWith("builtin-"))
            return;
        exportController.removePreset(String(preset.id));
        selectPresetId("");
    }

    function options() {
        const parsedEdge = Number(sizeField.text);
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
        };
    }

    background: Rectangle {
        radius: Theme.controlRadius + 2
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    FolderDialog {
        id: destinationFolderDialog
        title: qsTr("Choose export folder")
        onAccepted: dialog.exportController.startExport(dialog.targets, selectedFolder, dialog.options())
    }

    FileDialog {
        id: watermarkFileDialog
        title: qsTr("Choose a PNG watermark")
        nameFilters: [qsTr("PNG images (*.png)")]
        onAccepted: dialog.watermarkPath = selectedFile.toString()
    }

    Popup {
        id: presetNamePopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: 340
        padding: 16
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.panelRaised
            radius: Theme.controlRadius
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                text: qsTr("SAVE EXPORT PRESET")
                color: Theme.textPrimary
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 0.8
            }

            TextField {
                id: presetNameField
                Layout.fillWidth: true
                placeholderText: qsTr("Preset name")
                color: Theme.textPrimary
                placeholderTextColor: Theme.textPlaceholder
                selectByMouse: true
                background: Rectangle {
                    color: Theme.control
                    radius: Theme.compactControlRadius
                    border.width: 1
                    border.color: presetNameField.activeFocus ? Theme.focusRing : Theme.border
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item {
                    Layout.fillWidth: true
                }
                ShadowButton {
                    text: qsTr("CANCEL")
                    variant: ShadowButton.Ghost
                    onClicked: presetNamePopup.close()
                }
                ShadowButton {
                    text: qsTr("SAVE")
                    variant: ShadowButton.Primary
                    enabled: presetNameField.text.trim().length > 0
                    onClicked: {
                        const presetId = dialog.exportController.savePreset(
                            presetNameField.text, dialog.options());
                        presetNamePopup.close();
                        dialog.selectPresetId(presetId);
                    }
                }
            }
        }
    }

    Popup {
        id: presetRemovalPopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(390, parent.width - 40)
        padding: 16
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.panelRaised
            radius: Theme.controlRadius
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                text: qsTr("REMOVE PRESET")
                color: Theme.textPrimary
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 0.8
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Remove “%1”?").arg(
                          String(dialog.presetPendingRemoval
                                 ? dialog.presetPendingRemoval.name : ""))
                color: Theme.textPrimary
                font.pixelSize: 13
                font.weight: Font.Medium
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Only this local preset will be removed. Exported files and other presets are unchanged.")
                color: Theme.textMuted
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Item { Layout.fillWidth: true }
                ShadowButton {
                    text: qsTr("CANCEL")
                    variant: ShadowButton.Ghost
                    onClicked: presetRemovalPopup.close()
                }
                ShadowButton {
                    text: qsTr("REMOVE")
                    variant: ShadowButton.Danger
                    onClicked: dialog.removePendingPreset()
                }
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 12
            Layout.topMargin: 15
            Layout.bottomMargin: 13
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: qsTr("EXPORT")
                    color: Theme.textPrimary
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.0
                }
                Label {
                    text: qsTr("%L1 selected photos").arg(dialog.targets.length)
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
                Label {
                    text: qsTr("Saved locally · unfinished exports resume automatically")
                    color: Theme.textMuted
                    font.pixelSize: 9
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            ShadowIconButton {
                source: "qrc:/icons/clear.svg"
                toolTipText: qsTr("Close")
                accessibleName: toolTipText
                enabled: !dialog.exportController.busy
                onClicked: dialog.close()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            enabled: !dialog.exportController.busy

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
                                model: dialog.exportController.presets
                                textRole: "name"
                                valueRole: "id"
                                implicitHeight: Theme.controlHeight
                                onActivated: dialog.applyPreset(dialog.presetAt(currentIndex))
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
                                onClicked: {
                                    presetNameField.text = "";
                                    presetNamePopup.open();
                                    presetNameField.forceActiveFocus();
                                }
                            }

                            ShadowIconButton {
                                visible: dialog.selectedPresetIsCustom()
                                source: "qrc:/icons/trash.svg"
                                toolTipText: qsTr("Remove selected export preset")
                                accessibleName: toolTipText
                                variant: ShadowIconButton.Ghost
                                onClicked: dialog.requestPresetRemoval()
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
                                active: dialog.format === "jpeg"
                                onClicked: dialog.format = "jpeg"
                            }
                            ShadowTabButton {
                                Layout.fillWidth: true
                                text: "PNG"
                                active: dialog.format === "png"
                                onClicked: dialog.format = "png"
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
                            visible: dialog.format === "jpeg"
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
                                stepSize: 1
                                value: dialog.quality
                                onMoved: dialog.quality = Math.round(value)
                            }
                            Label {
                                Layout.preferredWidth: 30
                                text: String(dialog.quality)
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
                                onTextChanged: dialog.filenameSuffix = text
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
                                text: dialog.watermarkPath.length > 0 ? dialog.watermarkPath.split("/").pop() : qsTr("CHOOSE PNG")
                                variant: ShadowButton.Secondary
                                onClicked: watermarkFileDialog.open()
                            }
                            ShadowIconButton {
                                source: "qrc:/icons/clear.svg"
                                toolTipText: qsTr("Remove watermark")
                                accessibleName: toolTipText
                                enabled: dialog.watermarkPath.length > 0
                                onClicked: dialog.watermarkPath = ""
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
                                    color: dialog.watermarkAnchor === modelData ? Theme.accentSurface : Theme.controlQuiet
                                    border.width: dialog.watermarkAnchor === modelData ? 1 : 0
                                    border.color: Theme.accentBorder
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 5
                                        height: 5
                                        radius: width / 2
                                        color: dialog.watermarkAnchor === anchorCell.modelData ? Theme.accent : Theme.textMuted
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: dialog.watermarkAnchor = anchorCell.modelData
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
                                stepSize: 0.01
                                value: dialog.watermarkOpacity
                                onMoved: dialog.watermarkOpacity = value
                            }
                            Label {
                                Layout.preferredWidth: 36
                                text: qsTr("%1%").arg(Math.round(dialog.watermarkOpacity * 100))
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
                                stepSize: 0.01
                                value: dialog.watermarkScale
                                onMoved: dialog.watermarkScale = value
                            }
                            Label {
                                Layout.preferredWidth: 36
                                text: qsTr("%1%").arg(Math.round(dialog.watermarkScale * 100))
                                color: Theme.textMuted
                                font.pixelSize: 10
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 10
            Layout.bottomMargin: 10
            Layout.preferredHeight: Math.min(
                                      132,
                                      80 + Math.max(0, dialog.exportController.errors.length - 1) * 26
                                  )
            visible: !dialog.exportController.busy
                     && dialog.exportController.errors.length > 0
            radius: Theme.compactControlRadius
            color: Theme.dangerSurface
            border.width: 1
            border.color: Theme.errorBorder

            ColumnLayout {
                id: errorDetails
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6

                Label {
                    text: qsTr("FAILED ITEMS · %1").arg(dialog.exportController.errors.length)
                    color: Theme.dangerText
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.55
                }

                ListView {
                    id: errorList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 42
                    clip: true
                    spacing: 4
                    model: dialog.exportController.errors
                    delegate: Label {
                        required property string modelData
                        width: errorList.width
                        text: modelData
                        color: Theme.textSecondary
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 12
            Layout.bottomMargin: 12
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    BusyIndicator {
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        visible: dialog.exportController.busy
                        running: visible
                    }

                    Label {
                        Layout.fillWidth: true
                        text: dialog.exportController.statusText
                        color: dialog.exportController.errors.length > 0
                               && !dialog.exportController.busy
                               ? Theme.errorText : Theme.textMuted
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                }

                ProgressBar {
                    id: exportProgress
                    Layout.fillWidth: true
                    Layout.preferredHeight: 4
                    visible: dialog.exportController.busy
                    from: 0
                    to: Math.max(1, dialog.exportController.totalCount)
                    value: Math.min(
                               dialog.exportController.currentCount,
                               dialog.exportController.totalCount
                           )
                    background: Rectangle {
                        radius: height / 2
                        color: Theme.controlQuiet
                    }
                    contentItem: Item {
                        Rectangle {
                            width: parent.width * exportProgress.visualPosition
                            height: parent.height
                            radius: height / 2
                            color: dialog.exportController.cancellationRequested
                                   ? Theme.textMuted : Theme.accent
                        }
                    }
                }
            }

            ShadowButton {
                text: qsTr("CANCEL")
                variant: ShadowButton.Ghost
                enabled: !dialog.exportController.cancellationRequested
                onClicked: {
                    if (dialog.exportController.busy)
                        dialog.exportController.cancelExport();
                    else
                        dialog.close();
                }
            }

            ShadowButton {
                text: dialog.exportController.busy ? qsTr("EXPORTING…") : qsTr("EXPORT")
                variant: ShadowButton.Primary
                enabled: !dialog.exportController.busy && dialog.targets.length > 0
                onClicked: destinationFolderDialog.open()
            }
        }
    }

    Connections {
        target: dialog.exportController
        function onExportFinished(completed, failed, cancelled, paths, errors) {
            if (!cancelled && failed === 0 && completed > 0)
                dialog.close();
        }
    }
}
