pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Photo-local framing controls paired with the direct-manipulation crop
// overlay. Aspect is a transient constraint; normalized crop bounds and
// orientation remain the persisted v1 Recipe authority.
ColumnLayout {
    id: geometry

    required property var inspector
    required property int currentTabIndex
    required property real aspectRatioLock

    signal aspectRatioRequested(real ratio)

    spacing: 0

    ShadowAdjustmentSection {
        Layout.fillWidth: true
        visible: geometry.currentTabIndex === 0
        title: qsTr("CROP")
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

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            label: qsTr("Straighten")
            toolTipText: qsTr("Rotates the photo and automatically crops empty corners while preserving the current aspect ratio.")
            from: -45
            to: 45
            neutralValue: 0
            stepSize: 0.1
            decimals: 1
            suffix: "°"
            value: Number(
                geometry.inspector.editor.photoGeometry.straightenDegrees || 0)
            enabled: geometry.inspector.previewFrameReady
                && !geometry.inspector.editor.stateBusy
            onGestureStarted: geometry.inspector.editor.beginParameterEdit(
                "geometry/straighten")
            onEdited: value =>
                geometry.inspector.editor.setPhotoStraightenDegrees(value)
            onGestureFinished: geometry.inspector.editor.endParameterEdit(
                "geometry/straighten")
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 2
            text: qsTr("Empty corners are cropped automatically after rotation.")
            color: Theme.textMuted
            font.pixelSize: 9
            wrapMode: Text.WordWrap
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 6
            text: qsTr("ASPECT")
            toolTipText: qsTr("Freeform by default. A selected ratio constrains the canvas handles without becoming separate Recipe state.")
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 4

            Repeater {
                model: [
                    { "label": qsTr("Free"), "aspect": 0.0 },
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
                    selected: Math.abs(
                        geometry.aspectRatioLock - modelData.aspect) < 0.0001
                    enabled: geometry.inspector.previewFrameReady
                    onClicked: {
                        geometry.aspectRatioRequested(modelData.aspect)
                        if (modelData.aspect > 0) {
                            geometry.inspector.editor.setCenteredPhotoCropAspectRatio(
                                modelData.aspect,
                                geometry.inspector.currentPhotoAspect)
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            text: qsTr("Drag the frame, edges, or corners directly on the photo.")
            color: Theme.textMuted
            font.pixelSize: 9
            wrapMode: Text.WordWrap
        }
    }
}
