pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: settings
    required property var paint
    spacing: 8
    ShadowSlider {
        Layout.fillWidth: true
        label: qsTr("Roundness")
        from: 0.1; to: 1; neutralValue: 1; decimals: 0; displayMultiplier: 100; suffix: "%"
        value: settings.paint.roundness
        onEdited: value => settings.paint.roundness = value
    }
    ShadowSlider {
        Layout.fillWidth: true
        label: qsTr("Tip angle")
        from: -180; to: 180; neutralValue: 0; decimals: 0; suffix: "°"
        value: settings.paint.angle
        onEdited: value => settings.paint.angle = value
    }
    ShadowSlider {
        Layout.fillWidth: true
        label: qsTr("Spacing")
        from: 0.02; to: 1; neutralValue: 0.125; decimals: 1; displayMultiplier: 100; suffix: "%"
        value: settings.paint.spacing
        toolTipText: qsTr("Distance between impressions, relative to brush diameter. Lower spacing builds coverage faster.")
        onEdited: value => settings.paint.spacing = value
    }
    ShadowSlider {
        Layout.fillWidth: true
        label: qsTr("Smoothing")
        from: 0; to: 1; neutralValue: 0.2; decimals: 0; displayMultiplier: 100; suffix: "%"
        value: settings.paint.smoothing
        toolTipText: qsTr("Steadies the path while drawing. Release completes the stroke at the pointer.")
        onEdited: value => settings.paint.smoothing = value
    }
    Label { text: qsTr("Pressure"); color: Theme.textSecondary; font.pixelSize: Theme.fontMeta }
    RowLayout {
        ShadowCheckBox {
            text: qsTr("Size")
            checked: settings.paint.pressureSize
            onToggled: settings.paint.pressureSize = checked
        }
        ShadowCheckBox {
            text: qsTr("Flow")
            checked: settings.paint.pressureFlow
            onToggled: settings.paint.pressureFlow = checked
        }
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("A pen controls pressure; a mouse uses full pressure. Size ranges from 10% to the selected diameter.")
        font.pixelSize: Theme.fontMeta; color: Theme.textMuted; wrapMode: Text.WordWrap
    }
    ShadowComboBox {
        objectName: "paintTextureSelector"
        Layout.fillWidth: true
        model: [qsTr("Solid tip"), qsTr("Fine grain"), qsTr("Soft speckle")]
        currentIndex: settings.paint.texture
        onActivated: index => settings.paint.texture = index
    }
    ShadowSlider {
        Layout.fillWidth: true
        visible: settings.paint.texture !== 0
        label: qsTr("Texture strength")
        from: 0; to: 1; neutralValue: 0.5; decimals: 0; displayMultiplier: 100; suffix: "%"
        value: settings.paint.textureStrength
        onEdited: value => settings.paint.textureStrength = value
    }
}
