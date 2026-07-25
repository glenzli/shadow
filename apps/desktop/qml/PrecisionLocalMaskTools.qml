pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Spatial placement is owned by a Grade Node instance.  It is intentionally a
// small separate inspector region: the node's normal controls stay complete,
// while its mask can be created, adjusted, or removed without turning every
// tone/color control into a separate layer.
ColumnLayout {
    id: localMask

    required property var inspector
    required property int currentTabIndex

    readonly property var mask: inspector.editor.selectedLocalMask
    readonly property int kind: Number(mask.kind || 0)
    readonly property bool activeMask: kind === 1 || kind === 2

    spacing: 8

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: localMask.currentTabIndex === 0
        title: qsTr("LOCAL MASK")
        summary: localMask.activeMask
            ? (localMask.kind === 1 ? qsTr("Linear") : qsTr("Radial"))
            : qsTr("None")
        toolTipText: qsTr("Apply the selected Grade Node only within a linear or radial normalized region. The mask stays with this photo's node instance when the adjustment itself is shared.")
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
                toolTipText: qsTr("Blend the entire Grade Node along a straight gradient")
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
                toolTipText: qsTr("Blend the entire Grade Node inside a feathered ellipse")
                onClicked: localMask.inspector.editor.setSelectedLocalMask(2)
            }

            ShadowIconButton {
                visible: localMask.activeMask
                source: "qrc:/icons/trash.svg"
                toolTipText: qsTr("Remove local mask")
                accessibleName: toolTipText
                enabled: localMask.inspector.editor.active
                    && localMask.inspector.editor.hasSelectedGradeNode
                    && !localMask.inspector.editor.stateBusy
                onClicked: localMask.inspector.editor.setSelectedLocalMask(0)
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            visible: !localMask.activeMask
            text: qsTr("Choose a mask to constrain this complete adjustment node.")
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
                    : []
            delegate: ShadowSlider {
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                label: modelData.name
                from: modelData.from === undefined ? 0 : modelData.from
                to: 1
                neutralValue: 0.5
                stepSize: 0.01
                decimals: 0
                displayMultiplier: 100
                suffix: "%"
                value: Number(localMask.mask[modelData.key] || 0)
                enabled: localMask.inspector.editor.active
                    && !localMask.inspector.editor.stateBusy
                onGestureStarted: localMask.inspector.editor.beginParameterEdit("local_mask")
                onEdited: value => localMask.inspector.editor.setSelectedLocalMaskValue(
                    modelData.key, value)
                onGestureFinished: localMask.inspector.editor.endParameterEdit("local_mask")
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

            Switch {
                id: invertSwitch
                Layout.preferredWidth: 34
                Layout.preferredHeight: 20
                checked: Boolean(localMask.mask.inverted)
                enabled: localMask.inspector.editor.active
                    && !localMask.inspector.editor.stateBusy
                Accessible.name: qsTr("Invert local mask")
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
}
