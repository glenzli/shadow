pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Photo-local framing controls. The crop contract is already persisted by the
// backend, but this compact first surface exposes only transformations that
// have a complete, lossless canvas implementation. A crop overlay will join
// this group once its interactive source-space gesture is ready.
ColumnLayout {
    id: geometry

    required property var inspector
    required property int currentTabIndex

    spacing: 0

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: geometry.currentTabIndex === 0
        title: qsTr("TRANSFORM")
        summary: geometry.inspector.editor.photoGeometry.identity
            ? qsTr("Original") : qsTr("Adjusted")
        toolTipText: qsTr("Photo-local orientation. It is applied after grading and never becomes a shared Grade Node.")
        sectionEnabled: geometry.inspector.editor.active
            && !geometry.inspector.editor.stateBusy

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 6

            ShadowIconButton {
                source: "qrc:/icons/undo.svg"
                toolTipText: qsTr("Rotate counterclockwise")
                accessibleName: toolTipText
                onClicked: geometry.inspector.editor.rotatePhotoCounterClockwise()
            }

            ShadowIconButton {
                source: "qrc:/icons/redo.svg"
                toolTipText: qsTr("Rotate clockwise")
                accessibleName: toolTipText
                onClicked: geometry.inspector.editor.rotatePhotoClockwise()
            }

            ShadowIconButton {
                source: "qrc:/icons/flip-horizontal.svg"
                toolTipText: qsTr("Flip horizontally")
                accessibleName: toolTipText
                onClicked: geometry.inspector.editor.flipPhotoHorizontally()
            }

            ShadowIconButton {
                source: "qrc:/icons/flip-vertical.svg"
                toolTipText: qsTr("Flip vertically")
                accessibleName: toolTipText
                onClicked: geometry.inspector.editor.flipPhotoVertically()
            }

            Item { Layout.fillWidth: true }

            ShadowIconButton {
                source: "qrc:/icons/clear.svg"
                enabled: !geometry.inspector.editor.photoGeometry.identity
                toolTipText: qsTr("Reset transform")
                accessibleName: toolTipText
                onClicked: geometry.inspector.editor.resetPhotoGeometry()
            }
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 6
            text: qsTr("CROP")
            toolTipText: qsTr("Apply a centered crop ratio. Freeform crop handles will use the same photo-local geometry later.")
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 4

            Repeater {
                model: [
                    { "label": "1:1", "aspect": 1.0 },
                    { "label": "4:5", "aspect": 4.0 / 5.0 },
                    { "label": "3:2", "aspect": 3.0 / 2.0 },
                    { "label": "16:9", "aspect": 16.0 / 9.0 }
                ]

                delegate: ShadowButton {
                    required property var modelData

                    Layout.fillWidth: true
                    compact: true
                    minimumButtonWidth: 38
                    variant: ShadowButton.Ghost
                    text: modelData.label
                    enabled: geometry.inspector.previewFrameReady
                    onClicked: geometry.inspector.editor.setCenteredPhotoCropAspectRatio(
                        modelData.aspect, geometry.inspector.currentPhotoAspect)
                }
            }
        }
    }
}
