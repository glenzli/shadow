pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One selected repair region's mode and edge geometry. Collection navigation
// and creation stay in PrecisionRetouchTools.
ColumnLayout {
    id: regionInspector

    required property var editor
    required property var region
    required property bool continuous
    required property int displayIndex
    required property bool controlsEnabled

    Layout.fillWidth: true
    Layout.leftMargin: 14
    Layout.rightMargin: 14
    Layout.topMargin: 8
    Layout.bottomMargin: 4
    enabled: controlsEnabled
    spacing: 6

    function setMode(mode) {
        if (continuous) {
            editor.setRetouchStrokeMode(region.index, mode);
        } else {
            editor.setRetouchSpotMode(region.index, mode);
        }
    }

    function remove() {
        if (continuous) {
            editor.removeRetouchStroke(region.index);
        } else {
            editor.removeRetouchSpot(region.index);
        }
    }

    function setRadius(value) {
        if (continuous) {
            editor.setRetouchStrokeRadius(region.index, Math.round(value));
        } else {
            editor.setRetouchSpotRadius(region.index, Math.round(value));
        }
    }

    function setFeather(value) {
        if (continuous) {
            editor.setRetouchStrokeFeather(region.index, value);
        } else {
            editor.setRetouchSpotFeather(region.index, value);
        }
    }

    function setStrength(value) {
        if (continuous) {
            editor.setRetouchStrokeStrength(region.index, value);
        } else {
            editor.setRetouchSpotStrength(region.index, value);
        }
    }

    function resetSource() {
        if (continuous) {
            editor.setRetouchStrokeSourceOffset(region.index, 0, 0);
        } else {
            editor.setRetouchSpotSourceOffset(region.index, 0, 0);
        }
    }

    function setSourceTransform(rotation, scale, flipHorizontal, flipVertical) {
        if (continuous) {
            editor.setRetouchStrokeSourceTransform(
                region.index, rotation, scale, flipHorizontal, flipVertical);
        } else {
            editor.setRetouchSpotSourceTransform(
                region.index, rotation, scale, flipHorizontal, flipVertical);
        }
    }

    function historyKey(parameter) {
        return continuous ? "retouch/stroke/" + region.index + "/" + parameter : "retouch/" + region.index + "/" + parameter;
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: qsTr("Region %1").arg(regionInspector.displayIndex + 1)
            color: regionInspector.enabled ? Theme.textSecondary : Theme.textDisabled
            font.pixelSize: Theme.fontMeta
            font.weight: Font.DemiBold
        }

        ShadowIconButton {
            buttonSize: 30
            iconSize: 17
            source: "qrc:/icons/heal.svg"
            selected: Number(regionInspector.region.mode) !== 1
            toolTipText: qsTr("Blend a defect from its surrounding pixels")
            accessibleName: qsTr("Heal") + " · " + qsTr("Region %1").arg(regionInspector.displayIndex + 1)
            onClicked: regionInspector.setMode(0)
        }

        ShadowIconButton {
            buttonSize: 30
            iconSize: 17
            source: "qrc:/icons/clone.svg"
            selected: Number(regionInspector.region.mode) === 1
            toolTipText: qsTr("Copy a same-shaped nearby source")
            accessibleName: qsTr("Clone") + " · " + qsTr("Region %1").arg(regionInspector.displayIndex + 1)
            onClicked: regionInspector.setMode(1)
        }

        ShadowIconButton {
            buttonSize: 30
            iconSize: 16
            source: "qrc:/icons/trash.svg"
            variant: ShadowIconButton.Danger
            toolTipText: qsTr("Remove region %1").arg(regionInspector.displayIndex + 1)
            accessibleName: toolTipText
            onClicked: regionInspector.remove()
        }
    }

    RowLayout {
        objectName: "retouchHealBlendSelector"
        Layout.fillWidth: true
        spacing: 6
        visible: Number(regionInspector.region.mode) !== 1

        Label {
            text: qsTr("Blend")
            color: regionInspector.enabled ? Theme.textSecondary : Theme.textDisabled
            font.pixelSize: Theme.fontMeta
        }

        ShadowButton {
            objectName: "retouchNaturalHealButton"
            Layout.fillWidth: true
            compact: true
            selected: Number(regionInspector.region.mode) === 0
            text: qsTr("Natural")
            toolTipText: qsTr("Replace isolated spots without retaining their edges")
            onClicked: regionInspector.setMode(0)
        }

        ShadowButton {
            objectName: "retouchStructureHealButton"
            Layout.fillWidth: true
            compact: true
            selected: Number(regionInspector.region.mode) === 2
            text: qsTr("Structure")
            toolTipText: qsTr("Preserve strong lines and edges crossing the repair")
            onClicked: regionInspector.setMode(2)
        }
    }

    ShadowSlider {
        Layout.fillWidth: true
        label: qsTr("Size")
        from: 1
        to: 128
        neutralValue: 18
        stepSize: 1
        decimals: 0
        suffix: qsTr(" px")
        value: regionInspector.region.radius
        toolTipText: qsTr("Full-resolution repair radius")
        onGestureStarted: regionInspector.editor.beginParameterEdit(regionInspector.historyKey("radius"))
        onEdited: value => regionInspector.setRadius(value)
        onGestureFinished: regionInspector.editor.endParameterEdit(regionInspector.historyKey("radius"))
    }

    ShadowSlider {
        objectName: "retouchStrengthSlider"
        Layout.fillWidth: true
        label: qsTr("Strength")
        from: 0
        to: 1
        neutralValue: 1
        stepSize: 0.01
        decimals: 0
        displayMultiplier: 100
        suffix: "%"
        value: Number(regionInspector.region.strength)
        toolTipText: qsTr("Blend the repair with the original")
        onGestureStarted: regionInspector.editor.beginParameterEdit(regionInspector.historyKey("strength"))
        onEdited: value => regionInspector.setStrength(value)
        onGestureFinished: regionInspector.editor.endParameterEdit(regionInspector.historyKey("strength"))
    }

    ShadowSlider {
        Layout.fillWidth: true
        label: qsTr("Feather")
        from: 0
        to: 1
        neutralValue: 0.28
        stepSize: 0.01
        decimals: 0
        displayMultiplier: 100
        suffix: "%"
        value: Number(regionInspector.region.feather)
        toolTipText: qsTr("Soften the repair edge")
        onGestureStarted: regionInspector.editor.beginParameterEdit(regionInspector.historyKey("feather"))
        onEdited: value => regionInspector.setFeather(value)
        onGestureFinished: regionInspector.editor.endParameterEdit(regionInspector.historyKey("feather"))
    }

    ShadowSlider {
        objectName: "retouchSourceRotationSlider"
        Layout.fillWidth: true
        visible: Number(regionInspector.region.mode) === 1
        label: qsTr("Source rotation")
        from: -180
        to: 180
        neutralValue: 0
        stepSize: 1
        decimals: 0
        suffix: "°"
        value: Number(regionInspector.region.sourceRotation)
        toolTipText: qsTr("Rotate the sampled source around the target anchor")
        onGestureStarted: regionInspector.editor.beginParameterEdit(regionInspector.historyKey("source-transform"))
        onEdited: value => regionInspector.setSourceTransform(
            value,
            Number(regionInspector.region.sourceScale),
            Boolean(regionInspector.region.sourceFlipHorizontal),
            Boolean(regionInspector.region.sourceFlipVertical))
        onGestureFinished: regionInspector.editor.endParameterEdit(regionInspector.historyKey("source-transform"))
    }

    ShadowSlider {
        objectName: "retouchSourceScaleSlider"
        Layout.fillWidth: true
        visible: Number(regionInspector.region.mode) === 1
        label: qsTr("Source scale")
        from: 0.25
        to: 4
        neutralValue: 1
        stepSize: 0.01
        decimals: 0
        displayMultiplier: 100
        suffix: "%"
        value: Number(regionInspector.region.sourceScale)
        toolTipText: qsTr("Resize the sampled source texture")
        onGestureStarted: regionInspector.editor.beginParameterEdit(regionInspector.historyKey("source-transform"))
        onEdited: value => regionInspector.setSourceTransform(
            Number(regionInspector.region.sourceRotation),
            value,
            Boolean(regionInspector.region.sourceFlipHorizontal),
            Boolean(regionInspector.region.sourceFlipVertical))
        onGestureFinished: regionInspector.editor.endParameterEdit(regionInspector.historyKey("source-transform"))
    }

    RowLayout {
        Layout.fillWidth: true
        visible: Number(regionInspector.region.mode) === 1
        spacing: 6

        Label {
            Layout.fillWidth: true
            text: qsTr("Mirror source")
            color: regionInspector.enabled ? Theme.textSecondary : Theme.textDisabled
            font.pixelSize: Theme.fontMeta
        }

        ShadowButton {
            objectName: "retouchFlipSourceHorizontalButton"
            compact: true
            selected: Boolean(regionInspector.region.sourceFlipHorizontal)
            text: qsTr("Horizontal")
            onClicked: regionInspector.setSourceTransform(
                Number(regionInspector.region.sourceRotation),
                Number(regionInspector.region.sourceScale),
                !Boolean(regionInspector.region.sourceFlipHorizontal),
                Boolean(regionInspector.region.sourceFlipVertical))
        }

        ShadowButton {
            objectName: "retouchFlipSourceVerticalButton"
            compact: true
            selected: Boolean(regionInspector.region.sourceFlipVertical)
            text: qsTr("Vertical")
            onClicked: regionInspector.setSourceTransform(
                Number(regionInspector.region.sourceRotation),
                Number(regionInspector.region.sourceScale),
                Boolean(regionInspector.region.sourceFlipHorizontal),
                !Boolean(regionInspector.region.sourceFlipVertical))
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: qsTr("Source · %1 px").arg(Math.round(Math.hypot(Number(regionInspector.region.sourceOffsetX), Number(regionInspector.region.sourceOffsetY)) * Number(regionInspector.region.radius)))
            color: regionInspector.enabled ? Theme.textSecondary : Theme.textDisabled
            font.pixelSize: Theme.fontMeta
        }

        ShadowButton {
            objectName: "retouchResetSourceButton"
            compact: true
            text: qsTr("Reset source")
            toolTipText: qsTr("Choose a deterministic nearby source again")
            onClicked: regionInspector.resetSource()
        }
    }

    Label {
        Layout.fillWidth: true
        visible: true
        text: qsTr("Select this repair, then drag its outlined source region on the image.")
        color: regionInspector.enabled ? Theme.textMuted : Theme.textDisabled
        font.pixelSize: Theme.fontCaption
        lineHeight: 1.2
    }
}
