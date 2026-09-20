pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ColumnLayout {
    id: tools
    required property var editor
    readonly property var paint: editor.paint
    property bool advanced: false
    property bool layerControls: false
    spacing: 8
    Label {
        Layout.fillWidth: true
        text: qsTr("PAINT")
        font.pixelSize: Theme.fontBody; font.weight: Font.DemiBold; color: Theme.textPrimary
    }
    RowLayout {
        Layout.fillWidth: true
        enabled: !tools.paint.strokeActive
        ShadowComboBox {
            id: presets
            objectName: "paintPresetSelector"
            Layout.fillWidth: true
            model: tools.paint.presets
            textRole: "label"
            currentIndex: {
                for (let i=0; i<model.length; ++i) if (model[i].id === tools.paint.presetId) return i
                return -1
            }
            displayText: currentIndex < 0 ? qsTr("Custom brush") : currentText
            onActivated: index => tools.paint.applyPreset(model[index].id)
        }
        ShadowButton {
            implicitWidth: 36; text: "A"; selected: tools.paint.brushSlot === 0
            Accessible.name: qsTr("Brush A")
            onClicked: tools.paint.brushSlot = 0
            ToolTip.visible: hovered; ToolTip.text: qsTr("Switch brush A/B · X")
        }
        ShadowButton {
            implicitWidth: 36; text: "B"; selected: tools.paint.brushSlot === 1
            Accessible.name: qsTr("Brush B")
            onClicked: tools.paint.brushSlot = 1
            ToolTip.visible: hovered; ToolTip.text: qsTr("Switch brush A/B · X")
        }
    }
    RowLayout {
        Layout.fillWidth: true
        enabled: !tools.paint.strokeActive
        ShadowComboBox {
            objectName: "paintLayerSelector"
            Layout.fillWidth: true
            model: tools.paint.layers; textRole: "label"
            currentIndex: tools.paint.selectedIndex
            displayText: currentIndex < 0 ? qsTr("Draw to create a layer") : qsTr("%1 · %2").arg(currentIndex + 1).arg(currentText)
            enabled: count > 0
            onActivated: index => tools.paint.selectLayer(index)
        }
        ShadowIconButton {
            source: "qrc:/icons/node-add.svg"
            accessibleName: qsTr("Add paint layer"); toolTipText: accessibleName
            enabled: tools.paint.layers.length < 8
            onClicked: tools.paint.addLayer()
        }
        ShadowIconButton {
            source: "qrc:/icons/trash.svg"
            accessibleName: qsTr("Remove paint layer"); toolTipText: accessibleName
            enabled: tools.paint.selectedIndex >= 0
            onClicked: tools.paint.removeLayer()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        enabled: !tools.paint.strokeActive
        ShadowButton {
            objectName: "paintBrushMode"; Layout.fillWidth: true
            text: qsTr("Brush"); selected: !tools.paint.erase && !tools.paint.picking
            onClicked: tools.paint.erase = false
        }
        ShadowButton {
            objectName: "paintEraseMode"; Layout.fillWidth: true
            text: qsTr("Erase"); selected: tools.paint.erase && !tools.paint.picking
            onClicked: tools.paint.erase = true
        }
        ShadowIconButton {
            objectName: "paintEyedropper"
            source: "qrc:/icons/eyedropper.svg"; selected: tools.paint.picking
            accessibleName: qsTr("Sample paint color · Alt-click"); toolTipText: accessibleName
            onClicked: tools.paint.picking = !tools.paint.picking
        }
    }
    RowLayout {
        Layout.fillWidth: true
        enabled: !tools.paint.erase && !tools.paint.strokeActive
        ShadowButton {
            objectName: "paintColorButton"; Layout.fillWidth: true
            text: qsTr("Color…")
            onClicked: { colorDialog.selectedColor = tools.paint.color; colorDialog.open() }
        }
        Rectangle { implicitWidth: 32; implicitHeight: 26; radius: Theme.controlRadius; color: tools.paint.color; border.color: Theme.borderStrong }
        Repeater {
            model: ["#000000", "#808080", "#ffffff"]
            ShadowButton {
                required property string modelData
                readonly property string presetName: modelData === "#808080" ? qsTr("Neutral gray")
                    : modelData === "#000000" ? (tools.paint.brushBlend === 2 ? qsTr("Darken") : qsTr("Black"))
                    : (tools.paint.brushBlend === 2 ? qsTr("Lighten") : qsTr("White"))
                implicitWidth: 26; text: ""; Accessible.name: presetName
                Rectangle { anchors.centerIn: parent; width: 14; height: 14; radius: 3; color: parent.modelData; border.color: Theme.borderStrong }
                onClicked: tools.paint.color = modelData
                ToolTip.visible: hovered; ToolTip.text: presetName
            }
        }
    }
    ColorDialog { id: colorDialog; title: qsTr("Paint color"); onAccepted: tools.paint.color = selectedColor }
    ShadowSlider {
        Layout.fillWidth: true; enabled: !tools.paint.strokeActive
        label: qsTr("Brush size")
        from: 0.0001; to: 0.1; stepSize: 0.0001; neutralValue: 0.01
        decimals: 2; displayMultiplier: 200; suffix: "%"; value: tools.paint.radius
        toolTipText: qsTr("Diameter relative to the original photo's shorter edge. Use [ and ] to resize.")
        onEdited: value => tools.paint.radius = value
    }
    ShadowSlider {
        Layout.fillWidth: true; enabled: !tools.paint.strokeActive
        label: qsTr("Flow")
        from: 0.01; to: 1; neutralValue: 0.1; decimals: 0; displayMultiplier: 100; suffix: "%"; value: tools.paint.flow
        onEdited: value => tools.paint.flow = value
    }
    ShadowSlider {
        Layout.fillWidth: true; enabled: !tools.paint.strokeActive
        label: qsTr("Hardness")
        from: 0; to: 1; neutralValue: 0; decimals: 0; displayMultiplier: 100; suffix: "%"; value: tools.paint.hardness
        onEdited: value => tools.paint.hardness = value
    }
    ShadowSlider {
        Layout.fillWidth: true; enabled: !tools.paint.strokeActive
        label: qsTr("Opacity")
        from: 0; to: 1; neutralValue: 1; decimals: 0; displayMultiplier: 100; suffix: "%"; value: tools.paint.opacity
        onEdited: value => tools.paint.opacity = value
    }
    ShadowButton {
        Layout.fillWidth: true; text: qsTr("Brush tip and dynamics"); selected: tools.advanced
        onClicked: tools.advanced = !tools.advanced
    }
    PrecisionPaintBrushSettings {
        Layout.fillWidth: true; visible: tools.advanced; enabled: !tools.paint.strokeActive
        paint: tools.paint
    }
    RowLayout {
        Layout.fillWidth: true; visible: tools.advanced; enabled: !tools.paint.strokeActive
        ShadowButton { Layout.fillWidth: true; text: qsTr("Save brush…"); onClicked: { savedPresetName.text=""; saveDialog.open() } }
        ShadowButton {
            text: qsTr("Remove preset")
            enabled: presets.currentIndex >= 0 && presets.model[presets.currentIndex].custom
            onClicked: tools.paint.removePreset(tools.paint.presetId)
        }
    }
    Dialog {
        id: saveDialog; title: qsTr("Save brush preset"); modal: true; width: 280
        parent: Overlay.overlay
        anchors.centerIn: parent
        onOpened: savedPresetName.forceActiveFocus()
        standardButtons: Dialog.NoButton
        ColumnLayout {
            anchors.fill: parent
            ShadowTextField {
                id: savedPresetName
                Layout.fillWidth: true
                placeholderText: qsTr("Preset name"); maximumLength: 64
                onAccepted: { if (text.trim().length > 0 && tools.paint.savePreset(text)) saveDialog.close() }
            }
            Label { Layout.fillWidth: true; text: qsTr("A matching name replaces that preset."); wrapMode: Text.WordWrap; color: Theme.textMuted; font.pixelSize: Theme.fontMeta }
            RowLayout {
                ShadowButton { text: qsTr("Cancel"); onClicked: saveDialog.close() }
                ShadowButton { text: qsTr("Save"); enabled: savedPresetName.text.trim().length > 0; onClicked: { if (tools.paint.savePreset(savedPresetName.text)) saveDialog.close() } }
            }
        }
    }
    ShadowSlider {
        Layout.fillWidth: true; enabled: tools.paint.selectedIndex >= 0 && !tools.paint.strokeActive
        label: qsTr("Layer opacity")
        from: 0; to: 1; neutralValue: 1; decimals: 0; displayMultiplier: 100; suffix: "%"; value: tools.paint.layerOpacity
        onGestureStarted: tools.editor.beginParameterEdit("paint/layer/opacity")
        onEdited: value => tools.paint.layerOpacity = value
        onGestureFinished: tools.editor.endParameterEdit("paint/layer/opacity")
    }
    ShadowButton { Layout.fillWidth: true; text: qsTr("Layer controls"); selected: tools.layerControls; onClicked: tools.layerControls = !tools.layerControls }
    ColumnLayout {
        Layout.fillWidth: true; visible: tools.layerControls; enabled: !tools.paint.strokeActive
        RowLayout {
            Layout.fillWidth: true; enabled: tools.paint.selectedIndex >= 0
            ShadowCheckBox { text: qsTr("Layer visible"); checked: tools.paint.layerEnabled; onToggled: tools.paint.layerEnabled = checked }
            Item { Layout.fillWidth: true }
            ShadowIconButton { source: "qrc:/icons/move-down.svg"; accessibleName: qsTr("Move paint layer down"); toolTipText: accessibleName; enabled: tools.paint.selectedIndex > 0; onClicked: tools.paint.moveLayer(-1) }
            ShadowIconButton { source: "qrc:/icons/move-up.svg"; accessibleName: qsTr("Move paint layer up"); toolTipText: accessibleName; enabled: tools.paint.selectedIndex < tools.paint.layers.length - 1; onClicked: tools.paint.moveLayer(1) }
        }
        ShadowComboBox {
            objectName: "paintBlendSelector"; Layout.fillWidth: true
            model: [qsTr("Normal · cover"), qsTr("Color · preserve lightness"), qsTr("Soft light · shape light")]
            currentIndex: tools.paint.blend
            onActivated: index => tools.paint.blend = index
        }
    }
    Label {
        Layout.fillWidth: true; visible: tools.paint.picking || !tools.paint.canPaint || tools.paint.status.length > 0
        text: tools.paint.status.length > 0 ? tools.paint.status : tools.paint.picking ? qsTr("Click the photo to sample a color.") : qsTr("Enable the layer to paint.")
        color: Theme.textSecondary; font.pixelSize: Theme.fontMeta; wrapMode: Text.WordWrap
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("A preset with a different blend starts a new layer on the next stroke. Existing strokes keep their settings.")
        color: Theme.textMuted; font.pixelSize: Theme.fontMeta; wrapMode: Text.WordWrap
    }
}
