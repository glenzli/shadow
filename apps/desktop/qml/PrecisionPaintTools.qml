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
    spacing: 10
    Label {
        Layout.fillWidth: true
        text: tools.paint.dodgeBurn ? qsTr("DODGE & BURN") : qsTr("PAINT")
        font.pixelSize: Theme.fontBody; font.weight: Font.DemiBold; color: Theme.textPrimary
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        enabled: !tools.paint.strokeActive
        ShadowTabButton { Layout.fillWidth: true; text: qsTr("Paint"); active: !tools.paint.dodgeBurn; onClicked: tools.paint.dodgeBurn = false }
        ShadowTabButton { Layout.fillWidth: true; text: qsTr("Dodge & Burn"); active: tools.paint.dodgeBurn; onClicked: tools.paint.dodgeBurn = true }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: !tools.paint.dodgeBurn
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
            compact: true; minimumButtonWidth: 32; text: "A"; selected: tools.paint.brushSlot === 0
            variant: ShadowButton.Ghost
            Accessible.name: qsTr("Brush A")
            onClicked: tools.paint.brushSlot = 0
            ToolTip.visible: hovered; ToolTip.text: qsTr("Switch brush A/B · X")
        }
        ShadowButton {
            compact: true; minimumButtonWidth: 32; text: "B"; selected: tools.paint.brushSlot === 1
            variant: ShadowButton.Ghost
            Accessible.name: qsTr("Brush B")
            onClicked: tools.paint.brushSlot = 1
            ToolTip.visible: hovered; ToolTip.text: qsTr("Switch brush A/B · X")
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
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
            Layout.leftMargin: 4
            source: "qrc:/icons/trash.svg"
            accessibleName: qsTr("Remove paint layer"); toolTipText: accessibleName
            enabled: tools.paint.selectedIndex >= 0
            onClicked: tools.paint.removeLayer()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 4
        spacing: 8
        enabled: !tools.paint.strokeActive
        ShadowIconButton {
            visible: !tools.paint.dodgeBurn
            objectName: "paintBrushMode"
            buttonSize: 32; source: "qrc:/icons/brush.svg"
            accessibleName: qsTr("Brush"); toolTipText: accessibleName
            selected: !tools.paint.erase && !tools.paint.picking
            onClicked: tools.paint.erase = false
        }
        ShadowButton {
            objectName: "dodgeMode"
            visible: tools.paint.dodgeBurn
            compact: true; text: qsTr("Dodge"); selected: !tools.paint.burn && !tools.paint.erase
            onClicked: { tools.paint.erase = false; tools.paint.burn = false }
        }
        ShadowButton {
            objectName: "burnMode"
            visible: tools.paint.dodgeBurn
            compact: true; text: qsTr("Burn"); selected: tools.paint.burn && !tools.paint.erase
            onClicked: { tools.paint.erase = false; tools.paint.burn = true }
        }
        ShadowIconButton {
            objectName: "paintEraseMode"
            buttonSize: 32; source: "qrc:/icons/eraser.svg"
            accessibleName: qsTr("Erase"); toolTipText: accessibleName
            selected: tools.paint.erase && !tools.paint.picking
            onClicked: tools.paint.erase = true
        }
        ShadowIconButton {
            visible: !tools.paint.dodgeBurn
            objectName: "paintEyedropper"
            buttonSize: 32
            source: "qrc:/icons/eyedropper.svg"; selected: tools.paint.picking
            accessibleName: qsTr("Sample paint color · Alt-click"); toolTipText: accessibleName
            onClicked: tools.paint.picking = !tools.paint.picking
        }
        Item { Layout.fillWidth: true }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: !tools.paint.dodgeBurn
        enabled: !tools.paint.erase && !tools.paint.strokeActive
        ShadowButton {
            objectName: "paintColorButton"
            compact: true; minimumButtonWidth: 40
            accessibleName: qsTr("Color…"); toolTipText: accessibleName
            contentItem: Rectangle {
                implicitWidth: 20; implicitHeight: 18
                radius: 3; color: tools.paint.color; border.color: Theme.borderStrong
            }
            topPadding: 6; bottomPadding: 6
            onClicked: { colorDialog.selectedColor = tools.paint.color; colorDialog.open() }
        }
        Item { Layout.preferredWidth: 4 }
        Repeater {
            model: ["#000000", "#808080", "#ffffff"]
            ShadowButton {
                required property string modelData
                readonly property string presetName: modelData === "#808080" ? qsTr("Neutral gray")
                    : modelData === "#000000" ? (tools.paint.brushBlend === 2 ? qsTr("Darken") : qsTr("Black"))
                    : (tools.paint.brushBlend === 2 ? qsTr("Lighten") : qsTr("White"))
                compact: true; minimumButtonWidth: 32; text: ""; accessibleName: presetName
                variant: ShadowButton.Ghost
                Rectangle { anchors.centerIn: parent; width: 14; height: 14; radius: 3; color: parent.modelData; border.color: Theme.borderStrong }
                onClicked: tools.paint.color = modelData
                ToolTip.visible: hovered; ToolTip.text: presetName
            }
        }
        Item { Layout.fillWidth: true }
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
    ShadowAdjustmentSection {
        Layout.fillWidth: true; Layout.topMargin: 4
        title: qsTr("Brush tip and dynamics")
        expanded: false
        PrecisionPaintBrushSettings {
            Layout.fillWidth: true; enabled: !tools.paint.strokeActive
            paint: tools.paint
        }
        RowLayout {
            Layout.fillWidth: true; Layout.topMargin: 8
            enabled: !tools.paint.strokeActive
            spacing: 8
            ShadowButton { visible: !tools.paint.dodgeBurn; compact: true; text: qsTr("Save brush…"); onClicked: { savedPresetName.text=""; saveDialog.open() } }
            Item { Layout.fillWidth: true }
            ShadowIconButton {
                source: "qrc:/icons/trash.svg"
                accessibleName: qsTr("Remove preset"); toolTipText: accessibleName
                visible: !tools.paint.dodgeBurn
                enabled: presets.currentIndex >= 0 && presets.model[presets.currentIndex].custom
                onClicked: tools.paint.removePreset(tools.paint.presetId)
            }
        }
    }
    ShadowDialog {
        id: saveDialog; title: qsTr("Save brush preset"); modal: true; width: 280
        parent: Overlay.overlay
        anchors.centerIn: parent
        onOpened: savedPresetName.forceActiveFocus()
        standardButtons: Dialog.NoButton
        contentItem: ColumnLayout {
            spacing: 12
            ShadowTextField {
                id: savedPresetName
                Layout.fillWidth: true
                placeholderText: qsTr("Preset name"); maximumLength: 64
                onAccepted: { if (text.trim().length > 0 && tools.paint.savePreset(text)) saveDialog.close() }
            }
            Label { Layout.fillWidth: true; text: qsTr("A matching name replaces that preset."); wrapMode: Text.WordWrap; color: Theme.textMuted; font.pixelSize: Theme.fontMeta }
            RowLayout {
                ShadowButton { text: qsTr("Cancel"); onClicked: saveDialog.close() }
                ShadowButton { variant: ShadowButton.Primary; text: qsTr("Save"); enabled: savedPresetName.text.trim().length > 0; onClicked: { if (tools.paint.savePreset(savedPresetName.text)) saveDialog.close() } }
            }
        }
    }
    ShadowSlider {
        Layout.fillWidth: true; enabled: tools.paint.selectedIndex >= 0 && !tools.paint.strokeActive
        label: tools.paint.dodgeBurn ? qsTr("Strength") : qsTr("Layer opacity")
        from: 0; to: 1; neutralValue: 1; decimals: 0; displayMultiplier: 100; suffix: "%"; value: tools.paint.layerOpacity
        onGestureStarted: tools.editor.beginParameterEdit("paint/layer/opacity")
        onEdited: value => tools.paint.layerOpacity = value
        onGestureFinished: tools.editor.endParameterEdit("paint/layer/opacity")
    }
    ShadowAdjustmentSection {
        Layout.fillWidth: true; Layout.topMargin: 4
        title: qsTr("Layer controls")
        expanded: false
        RowLayout {
            Layout.fillWidth: true
            enabled: !tools.paint.strokeActive && tools.paint.selectedIndex >= 0
            spacing: 8
            ShadowCheckBox { text: qsTr("Layer visible"); checked: tools.paint.layerEnabled; onToggled: tools.paint.layerEnabled = checked }
            Item { Layout.fillWidth: true }
            ShadowIconButton { source: "qrc:/icons/move-down.svg"; accessibleName: qsTr("Move paint layer down"); toolTipText: accessibleName; enabled: tools.paint.selectedIndex > 0; onClicked: tools.paint.moveLayer(-1) }
            ShadowIconButton { source: "qrc:/icons/move-up.svg"; accessibleName: qsTr("Move paint layer up"); toolTipText: accessibleName; enabled: tools.paint.selectedIndex < tools.paint.layers.length - 1; onClicked: tools.paint.moveLayer(1) }
        }
        ShadowComboBox {
            visible: !tools.paint.dodgeBurn
            objectName: "paintBlendSelector"; Layout.fillWidth: true
            Layout.topMargin: 8
            enabled: !tools.paint.strokeActive
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
        text: tools.paint.dodgeBurn ? qsTr("Shape perceptual lightness with a soft, low-flow brush. Hold Alt to reverse; X switches Dodge/Burn. Color axes stay unchanged.") : qsTr("A preset with a different blend starts a new layer on the next stroke. Existing strokes keep their settings.")
        color: Theme.textMuted; font.pixelSize: Theme.fontMeta; wrapMode: Text.WordWrap
    }
}
