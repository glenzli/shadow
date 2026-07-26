pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A Grade Node owns at most one spatial-mask attachment. It is intentionally a
// small separate inspector region: the node's normal controls stay complete,
// while its one mask can be created, adjusted, or removed without growing a
// nested mask/layer tree.
ColumnLayout {
    id: localMask

    required property var inspector
    required property int currentTabIndex

    readonly property var mask: inspector.editor.selectedLocalMask
    readonly property int kind: Number(mask.kind || 0)
    readonly property bool activeMask: kind >= 1 && kind <= 3

    spacing: 8

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: localMask.currentTabIndex === 0
        title: qsTr("NODE MASK")
        summary: localMask.activeMask
            ? (localMask.kind === 1
                ? qsTr("Linear")
                : localMask.kind === 2 ? qsTr("Radial") : qsTr("Brush"))
            : qsTr("None")
        toolTipText: qsTr("Limit this entire Grade Node with one editable photo-local mask.")
        sectionEnabled: localMask.inspector.editor.active
            && localMask.inspector.editor.hasSelectedGradeNode
            && !localMask.inspector.editor.stateBusy

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Linear")
                selected: localMask.kind === 1
                enabled: localMask.inspector.editor.active
                    && localMask.inspector.editor.hasSelectedGradeNode
                    && !localMask.inspector.editor.stateBusy
                toolTipText: qsTr("Limit this entire Grade Node along a straight gradient")
                onClicked: localMask.inspector.editor.setSelectedLocalMask(1)
            }

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Radial")
                selected: localMask.kind === 2
                enabled: localMask.inspector.editor.active
                    && localMask.inspector.editor.hasSelectedGradeNode
                    && !localMask.inspector.editor.stateBusy
                toolTipText: qsTr("Limit this entire Grade Node inside a feathered ellipse")
                onClicked: localMask.inspector.editor.setSelectedLocalMask(2)
            }

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Brush")
                selected: localMask.kind === 3
                enabled: localMask.inspector.editor.active
                    && localMask.inspector.editor.hasSelectedGradeNode
                    && !localMask.inspector.editor.stateBusy
                toolTipText: qsTr("Paint the one mask that limits this entire Grade Node")
                onClicked: localMask.inspector.editor.setSelectedLocalMask(3)
            }

            ShadowIconButton {
                visible: localMask.activeMask
                source: "qrc:/icons/trash.svg"
                toolTipText: qsTr("Remove node mask")
                accessibleName: toolTipText
                enabled: localMask.inspector.editor.active
                    && localMask.inspector.editor.hasSelectedGradeNode
                    && !localMask.inspector.editor.stateBusy
                onClicked: localMask.inspector.editor.setSelectedLocalMask(0)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                visible: localMask.activeMask
                text: qsTr("Copy mask")
                enabled: localMask.inspector.editor.active
                    && !localMask.inspector.editor.stateBusy
                toolTipText: qsTr("Copy this normalized Node Mask geometry for another photo or Grade Node. Adjustment controls are not copied.")
                onClicked: localMask.inspector.editor.copySelectedLocalMask()
            }

            ShadowButton {
                compact: true
                Layout.fillWidth: true
                text: qsTr("Paste mask")
                enabled: localMask.inspector.editor.active
                    && localMask.inspector.editor.hasCopiedNodeMask
                    && !localMask.inspector.editor.stateBusy
                toolTipText: qsTr("Replace this Grade Node's one mask with the copied geometry. The node keeps its own adjustments and Recipe.")
                onClicked: localMask.inspector.editor.pasteSelectedLocalMask()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 5

            Label {
                text: qsTr("MASK ASSETS")
                color: Theme.textMuted
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 0.7
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                ComboBox {
                    id: assetBox
                    Layout.fillWidth: true
                    model: localMask.inspector.editor.nodeMaskAssets
                    textRole: "name"
                    valueRole: "id"
                    enabled: model.length > 0
                    implicitHeight: Theme.controlHeight
                    contentItem: Label {
                        leftPadding: 10
                        rightPadding: 28
                        verticalAlignment: Text.AlignVCenter
                        text: assetBox.displayText
                        color: assetBox.enabled ? Theme.textPrimary : Theme.textMuted
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    background: Rectangle {
                        color: Theme.control
                        radius: Theme.compactControlRadius
                        border.width: 1
                        border.color: assetBox.activeFocus ? Theme.focusRing : Theme.border
                    }
                }

                ShadowIconButton {
                    source: "qrc:/icons/node-add.svg"
                    toolTipText: qsTr("Save this node mask geometry as a named local asset")
                    accessibleName: toolTipText
                    enabled: localMask.activeMask
                        && localMask.inspector.editor.active
                        && !localMask.inspector.editor.stateBusy
                    onClicked: {
                        assetNameField.text = "";
                        assetNamePopup.open();
                        assetNameField.forceActiveFocus();
                    }
                }

                ShadowButton {
                    compact: true
                    text: qsTr("Apply")
                    toolTipText: qsTr("Copy this asset into the selected Grade Node's one mask. Existing photo edits remain independent.")
                    enabled: assetBox.currentIndex >= 0
                        && localMask.inspector.editor.active
                        && !localMask.inspector.editor.stateBusy
                    onClicked: localMask.inspector.editor.applySelectedLocalMaskAsset(
                        String(assetBox.currentValue || ""))
                }

                ShadowIconButton {
                    visible: assetBox.currentIndex >= 0
                    source: "qrc:/icons/trash.svg"
                    toolTipText: qsTr("Remove selected local mask asset")
                    accessibleName: toolTipText
                    variant: ShadowIconButton.Ghost
                    onClicked: localMask.inspector.editor.removeLocalMaskAsset(
                        String(assetBox.currentValue || ""))
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Assets save normalized geometry only. Applying one replaces this node’s single mask and never changes other photos.")
                color: Theme.textMuted
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: !localMask.activeMask
            text: qsTr("Choose one mask to limit this complete adjustment node.")
            color: Theme.textMuted
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: localMask.kind === 1
                ? [
                    { "key": "x0", "name": qsTr("Start X") },
                    { "key": "y0", "name": qsTr("Start Y") },
                    { "key": "x1", "name": qsTr("End X") },
                    { "key": "y1", "name": qsTr("End Y") }
                ]
                : localMask.kind === 2
                    ? [
                        { "key": "x0", "name": qsTr("Center X") },
                        { "key": "y0", "name": qsTr("Center Y") },
                        { "key": "radiusX", "name": qsTr("Radius X"), "from": 0.01 },
                        { "key": "radiusY", "name": qsTr("Radius Y"), "from": 0.01 },
                        { "key": "feather", "name": qsTr("Feather") }
                    ]
                    : localMask.kind === 3
                        ? [
                            {
                                "key": "radiusX",
                                "name": qsTr("Size"),
                                "from": 0.005,
                                "neutral": 0.035
                            },
                            {
                                "key": "feather",
                                "name": qsTr("Feather"),
                                "neutral": 0.6
                            }
                        ]
                    : []
            delegate: ShadowSlider {
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                label: modelData.name
                from: modelData.from === undefined ? 0 : modelData.from
                to: 1
                neutralValue: modelData.neutral === undefined
                    ? 0.5 : modelData.neutral
                stepSize: modelData.key === "radiusX"
                    && localMask.kind === 3 ? 0.005 : 0.01
                decimals: 0
                displayMultiplier: 100
                suffix: "%"
                value: Number(localMask.mask[modelData.key] || 0)
                enabled: localMask.inspector.editor.active
                    && !localMask.inspector.editor.stateBusy
                onGestureStarted: localMask.inspector.editor.beginParameterEdit(
                    "local_mask/" + modelData.key)
                onEdited: value => localMask.inspector.editor.setSelectedLocalMaskValue(
                    modelData.key, value)
                onGestureFinished: localMask.inspector.editor.endParameterEdit(
                    "local_mask/" + modelData.key)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: localMask.activeMask
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Invert")
                color: Theme.textSecondary
                font.pixelSize: 10
            }

            ShadowIconButton {
                visible: localMask.kind === 3
                source: "qrc:/icons/clear.svg"
                toolTipText: qsTr("Clear brush strokes")
                accessibleName: toolTipText
                enabled: localMask.inspector.editor.active
                    && !localMask.inspector.editor.stateBusy
                    && (localMask.mask.brushPoints || []).length > 0
                onClicked: localMask.inspector.editor.clearSelectedLocalMaskBrush()
            }

            Switch {
                id: invertSwitch
                Layout.preferredWidth: 34
                Layout.preferredHeight: 20
                checked: Boolean(localMask.mask.inverted)
                enabled: localMask.inspector.editor.active
                    && !localMask.inspector.editor.stateBusy
                Accessible.name: qsTr("Invert node mask")
                onClicked: localMask.inspector.editor.setSelectedLocalMaskInverted(checked)
                indicator: Rectangle {
                    implicitWidth: 34
                    implicitHeight: 18
                    x: (invertSwitch.width - width) / 2
                    y: (invertSwitch.height - height) / 2
                    radius: height / 2
                    color: invertSwitch.checked
                        ? Theme.switchOnSurface : Theme.switchOffSurface
                    border.color: invertSwitch.checked
                        ? Theme.switchOnBorder : Theme.switchOffBorder
                    Rectangle {
                        width: 12
                        height: 12
                        y: 3
                        x: invertSwitch.checked ? parent.width - width - 3 : 3
                        radius: width / 2
                        color: invertSwitch.checked
                            ? localMask.inspector.accent : Theme.textMuted
                    }
                }
                contentItem: Item {}
            }
        }
    }

    Popup {
        id: assetNamePopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(340, parent.width - 40)
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
                text: qsTr("SAVE MASK ASSET")
                color: Theme.textPrimary
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.letterSpacing: 0.8
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("A matching name updates that asset. Applied masks stay as independent Recipe snapshots.")
                color: Theme.textMuted
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }

            TextField {
                id: assetNameField
                Layout.fillWidth: true
                placeholderText: qsTr("Asset name")
                color: Theme.textPrimary
                placeholderTextColor: Theme.textPlaceholder
                selectByMouse: true
                background: Rectangle {
                    color: Theme.control
                    radius: Theme.compactControlRadius
                    border.width: 1
                    border.color: assetNameField.activeFocus
                        ? Theme.focusRing : Theme.border
                }
                onAccepted: {
                    if (text.trim().length > 0) {
                        localMask.inspector.editor.saveSelectedLocalMaskAsset(text);
                        assetNamePopup.close();
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ShadowButton {
                    text: qsTr("CANCEL")
                    variant: ShadowButton.Ghost
                    onClicked: assetNamePopup.close()
                }
                ShadowButton {
                    text: qsTr("SAVE")
                    variant: ShadowButton.Primary
                    enabled: assetNameField.text.trim().length > 0
                    onClicked: {
                        localMask.inspector.editor.saveSelectedLocalMaskAsset(
                            assetNameField.text);
                        assetNamePopup.close();
                    }
                }
            }
        }
    }
}
