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

    function croppedOutputAspect() {
        // The crop canvas displays the complete oriented source, while the
        // controller's centered-crop operation expects the current rectangle.
        // Passing the full-image aspect repeatedly shrinks the wrong axis.
        const crop = inspector.editor.photoGeometry
        const width = Number(crop.cropRight) - Number(crop.cropLeft)
        const height = Number(crop.cropBottom) - Number(crop.cropTop)
        const oddTurn = Number(crop.quarterTurn || 0) % 2 !== 0
        return inspector.currentPhotoAspect
            * (oddTurn ? height / width : width / height)
    }

    // A quarter turn preserves the source-space crop, so its output aspect
    // reciprocates. Keep the next handle gesture in that same output space,
    // including rotations reached through Undo/Redo.
    property int observedQuarterTurn: 0
    Component.onCompleted: observedQuarterTurn = Number(inspector.editor.photoGeometry.quarterTurn || 0)
    Connections {
        target: geometry.inspector.editor
        function onParametersChanged() {
            const turn = Number(geometry.inspector.editor.photoGeometry.quarterTurn || 0)
            const transposed = Math.abs(turn - geometry.observedQuarterTurn) % 2 === 1
            geometry.observedQuarterTurn = turn
            if (transposed && geometry.aspectRatioLock > 0)
                geometry.aspectRatioRequested(1 / geometry.aspectRatioLock)
        }
    }

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
        resetAvailable: true
        resetEnabled: !geometry.inspector.editor.photoGeometry.identity
            || geometry.aspectRatioLock > 0
        resetToolTipText: qsTr("Reset transform")
        onResetRequested: {
            geometry.aspectRatioRequested(0.0)
            geometry.inspector.editor.resetPhotoGeometry()
        }

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
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 6
            text: qsTr("AUTO GEOMETRY")
            toolTipText: qsTr("Detect dominant lines locally and preview a photo-level correction before changing the Recipe.")
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            columns: 2
            columnSpacing: 6
            rowSpacing: 6

            ShadowButton {
                objectName: "autoGeometryAutomaticButton"
                Layout.fillWidth: true
                compact: true
                text: geometry.inspector.editor.autoGeometryBusy
                    ? qsTr("Analyzing…") : qsTr("Auto")
                variant: ShadowButton.Primary
                enabled: geometry.inspector.editor.autoGeometryCanAnalyze
                toolTipText: qsTr("Choose reliable level and perspective corrections from the detected structure.")
                onClicked: geometry.inspector.editor.analyzeAutoGeometry(0)
            }

            ShadowButton {
                objectName: "autoGeometryLevelButton"
                Layout.fillWidth: true
                compact: true
                text: qsTr("Level")
                enabled: geometry.inspector.editor.autoGeometryCanAnalyze
                toolTipText: qsTr("Correct only the dominant horizon or near-horizontal lines.")
                onClicked: geometry.inspector.editor.analyzeAutoGeometry(1)
            }

            ShadowButton {
                objectName: "autoGeometryVerticalButton"
                Layout.fillWidth: true
                compact: true
                text: qsTr("Vertical")
                enabled: geometry.inspector.editor.autoGeometryCanAnalyze
                toolTipText: qsTr("Level the photo and straighten converging vertical lines.")
                onClicked: geometry.inspector.editor.analyzeAutoGeometry(2)
            }

            ShadowButton {
                objectName: "autoGeometryFullButton"
                Layout.fillWidth: true
                compact: true
                text: qsTr("Full")
                enabled: geometry.inspector.editor.autoGeometryCanAnalyze
                toolTipText: qsTr("Correct level plus both vertical and horizontal perspective.")
                onClicked: geometry.inspector.editor.analyzeAutoGeometry(3)
            }
        }

        Label {
            objectName: "autoGeometryStatusLabel"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            visible: text.length > 0
            text: geometry.inspector.editor.autoGeometryStatusText
            color: geometry.inspector.editor.autoGeometryHasProposal
                ? Theme.accent : Theme.textMuted
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
        }

        Label {
            objectName: "autoGeometryProposalLabel"
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 2
            visible: geometry.inspector.editor.autoGeometryHasProposal
            text: qsTr("Proposal · %1° · V %2% · H %3% · %4 lines")
                .arg(Number(
                    geometry.inspector.editor.autoGeometrySuggestedStraighten
                ).toFixed(1))
                .arg(Math.round(
                    Number(geometry.inspector.editor.autoGeometrySuggestedVertical) * 100
                ))
                .arg(Math.round(
                    Number(geometry.inspector.editor.autoGeometrySuggestedHorizontal) * 100
                ))
                .arg(geometry.inspector.editor.autoGeometrySupportingLines)
            color: Theme.textSecondary
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            visible: geometry.inspector.editor.autoGeometryHasProposal
            spacing: 6

            ShadowButton {
                objectName: "autoGeometryApplyButton"
                Layout.fillWidth: true
                compact: true
                text: qsTr("Apply")
                variant: ShadowButton.Primary
                enabled: geometry.inspector.editor.autoGeometryPreviewing
                    && !geometry.inspector.editor.autoGeometryBusy
                toolTipText: qsTr("Accept the preview as one undoable geometry edit.")
                onClicked: geometry.inspector.editor.acceptAutoGeometry()
            }

            ShadowButton {
                objectName: "autoGeometryCancelButton"
                Layout.fillWidth: true
                compact: true
                text: qsTr("Cancel")
                variant: ShadowButton.Ghost
                enabled: !geometry.inspector.editor.autoGeometryBusy
                toolTipText: qsTr("Discard the proposal and restore the authored geometry.")
                onClicked: geometry.inspector.editor.cancelAutoGeometry()
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            visible: !geometry.inspector.editor.autoGeometryCanAnalyze
                && !geometry.inspector.editor.autoGeometryBusy
                && !geometry.inspector.editor.autoGeometryHasProposal
                && (Number(
                    geometry.inspector.editor.photoGeometry.cropLeft || 0
                ) > 0.0001
                    || Number(
                        geometry.inspector.editor.photoGeometry.cropTop || 0
                    ) > 0.0001
                    || Math.abs(Number(
                        geometry.inspector.editor.photoGeometry.cropRight || 0
                    ) - 1) > 0.0001
                    || Math.abs(Number(
                        geometry.inspector.editor.photoGeometry.cropBottom || 0
                    ) - 1) > 0.0001
                    || Math.abs(Number(
                        geometry.inspector.editor.photoGeometry.straightenDegrees || 0
                    )) > 0.0001
                    || Math.abs(Number(
                        geometry.inspector.editor.photoGeometry.perspectiveVertical || 0
                    )) > 0.0001
                    || Math.abs(Number(
                        geometry.inspector.editor.photoGeometry.perspectiveHorizontal || 0
                    )) > 0.0001)
            text: qsTr("Reset Crop, Straighten, and Perspective before analyzing again.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
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
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
        }

        ShadowSubsectionLabel {
            Layout.topMargin: 6
            text: qsTr("PERSPECTIVE")
            toolTipText: qsTr("Correct converging vertical or horizontal lines with one photo-local projective transform.")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Vertical")
            from: -1
            to: 1
            neutralValue: 0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: Number(
                geometry.inspector.editor.photoGeometry.perspectiveVertical || 0)
            enabled: geometry.inspector.previewFrameReady
                && !geometry.inspector.editor.stateBusy
            onGestureStarted: geometry.inspector.editor.beginParameterEdit(
                "geometry/perspective")
            onEdited: value => geometry.inspector.editor.setPhotoPerspective(
                value,
                Number(geometry.inspector.editor.photoGeometry.perspectiveHorizontal || 0))
            onGestureFinished: geometry.inspector.editor.endParameterEdit(
                "geometry/perspective")
        }

        ShadowSlider {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            label: qsTr("Horizontal")
            from: -1
            to: 1
            neutralValue: 0
            stepSize: 0.01
            decimals: 0
            displayMultiplier: 100
            suffix: "%"
            value: Number(
                geometry.inspector.editor.photoGeometry.perspectiveHorizontal || 0)
            enabled: geometry.inspector.previewFrameReady
                && !geometry.inspector.editor.stateBusy
            onGestureStarted: geometry.inspector.editor.beginParameterEdit(
                "geometry/perspective")
            onEdited: value => geometry.inspector.editor.setPhotoPerspective(
                Number(geometry.inspector.editor.photoGeometry.perspectiveVertical || 0),
                value)
            onGestureFinished: geometry.inspector.editor.endParameterEdit(
                "geometry/perspective")
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
                    readonly property bool inverted: modelData.aspect > 0
                        && Math.abs(geometry.aspectRatioLock - 1 / modelData.aspect) < 0.0001
                    readonly property real effectiveAspect: inverted
                        ? 1 / modelData.aspect : modelData.aspect

                    objectName: "cropAspect" + modelData.label
                    Layout.fillWidth: true
                    compact: true
                    minimumButtonWidth: 38
                    variant: ShadowButton.Ghost
                    text: inverted ? String(modelData.label).split(":").reverse().join(":")
                        : modelData.label
                    selected: Math.abs(
                        geometry.aspectRatioLock - effectiveAspect) < 0.0001
                    enabled: geometry.inspector.previewFrameReady
                    onClicked: {
                        geometry.aspectRatioRequested(effectiveAspect)
                        if (effectiveAspect > 0) {
                            geometry.inspector.editor.setCenteredPhotoCropAspectRatio(
                                effectiveAspect,
                                geometry.croppedOutputAspect())
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
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.WordWrap
        }
    }
}
