pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: tools

    required property var editor
    property bool authoring: false
    signal startRequested
    signal refreshRequested(int index)
    signal exitRequested

    spacing: 10
    Layout.minimumWidth: 0

    function requestGeneration() {
        if (editor.imageCompletionExecutionAllowed) {
            editor.generateImageCompletion()
        } else {
            localExecutionDialog.open()
        }
    }

    Dialog {
        id: localExecutionDialog
        objectName: "imageCompletionLocalExecutionDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(500, parent.width - 48)
        modal: true
        implicitHeight: Math.max(230, localExecutionExplanation.implicitHeight + 124)
        title: qsTr("Allow local AI Completion?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: {
            tools.editor.imageCompletionExecutionAllowed = true
            tools.editor.generateImageCompletion()
        }

        contentItem: Label {
            id: localExecutionExplanation
            width: localExecutionDialog.availableWidth
            text: qsTr("Shadow will send a bounded crop of this photo and the selected area to Infer Runtime on this device. The request is not uploaded by Shadow. You can turn this permission off later in AI & Models settings.")
            color: Theme.textPrimary
            wrapMode: Text.WordWrap
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        text: qsTr("AI COMPLETION")
        color: Theme.textPrimary
        font.pixelSize: Theme.fontBody
        font.weight: Font.DemiBold
        font.letterSpacing: 0.25
    }

    Label {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        text: tools.authoring
            ? qsTr("Paint the area to replace. Generation creates a preview candidate; the photo Recipe changes only after Apply.")
            : qsTr("Adjust or remove accepted regions, or paint a new region to repair another area.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.Wrap
    }

    ShadowButton {
        objectName: "beginImageCompletionButton"
        Layout.fillWidth: true
        visible: !tools.authoring
        text: qsTr("Paint a new region")
        enabled: tools.editor.active && !tools.editor.stateBusy
        onClicked: tools.startRequested()
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: tools.authoring
        spacing: 9

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ShadowIconButton {
                objectName: "imageCompletionPaintButton"
                buttonSize: 32
                source: "qrc:/icons/brush.svg"
                toolTipText: qsTr("Paint")
                checkable: true
                checked: !tools.editor.imageCompletionEraseMode
                enabled: !tools.editor.imageCompletionBusy
                onClicked: tools.editor.imageCompletionEraseMode = false
            }

            ShadowIconButton {
                objectName: "imageCompletionEraseButton"
                buttonSize: 32
                source: "qrc:/icons/eraser.svg"
                toolTipText: qsTr("Erase")
                checkable: true
                checked: tools.editor.imageCompletionEraseMode
                enabled: !tools.editor.imageCompletionBusy
                onClicked: tools.editor.imageCompletionEraseMode = true
            }

            Item { Layout.fillWidth: true }

            ShadowIconButton {
                objectName: "imageCompletionUndoButton"
                buttonSize: 32
                source: "qrc:/icons/undo.svg"
                toolTipText: qsTr("Undo stroke")
                enabled: tools.editor.imageCompletionBrushPoints.length > 0
                    && !tools.editor.imageCompletionBusy
                onClicked: tools.editor.undoImageCompletionStroke()
            }

            ShadowIconButton {
                objectName: "imageCompletionClearButton"
                buttonSize: 32
                source: "qrc:/icons/clear.svg"
                toolTipText: qsTr("Clear")
                enabled: tools.editor.imageCompletionBrushPoints.length > 0
                    && !tools.editor.imageCompletionBusy
                onClicked: tools.editor.clearImageCompletionSelection()
            }
        }

        ShadowSlider {
            objectName: "imageCompletionBrushSize"
            Layout.fillWidth: true
            label: qsTr("Brush size")
            from: 0.002
            to: 0.25
            stepSize: 0.002
            neutralValue: 0.04
            decimals: 1
            displayMultiplier: 100
            suffix: "%"
            value: tools.editor.imageCompletionBrushRadius
            enabled: !tools.editor.imageCompletionBusy
            onEdited: value => tools.editor.imageCompletionBrushRadius = value
        }

        ShadowSlider {
            objectName: "imageCompletionSelectionExpansion"
            Layout.fillWidth: true
            label: qsTr("Selection expansion")
            from: 0
            to: 0.06
            stepSize: 0.002
            neutralValue: 0
            decimals: 1
            displayMultiplier: 100
            suffix: "%"
            value: tools.editor.imageCompletionSelectionExpansion
            enabled: tools.editor.imageCompletionBrushPoints.length > 0
                && !tools.editor.imageCompletionBusy
            onEdited: value => tools.editor.imageCompletionSelectionExpansion = value
        }

        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: qsTr("Expand a painted selection around soft edges, then generate again. Changing this discards the current candidate.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            visible: tools.editor.liquifyNodeMaterialized
                && tools.editor.liquifyNodeEnabled
            text: qsTr("Bypass Liquify while painting and generating a new AI Completion region. Accepted regions can be regenerated with Liquify enabled.")
            color: Theme.warningText
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.Wrap
        }

        Rectangle {
            objectName: "imageCompletionStatusCard"
            Layout.fillWidth: true
            implicitHeight: Math.max(18, candidateStatus.implicitHeight) + 20
            radius: Theme.controlRadius
            color: Theme.surfaceSubtle
            border.width: 1
            border.color: tools.editor.imageCompletionHasCandidate
                ? Theme.accentBorder : Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8

                BusyIndicator {
                    running: tools.editor.imageCompletionBusy
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }

                Label {
                    id: candidateStatus
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: tools.editor.imageCompletionBusy
                        ? qsTr("Generating a local candidate…")
                        : (tools.editor.imageCompletionHasCandidate
                            ? qsTr("Candidate preview is shown on the photo.")
                            : qsTr("No candidate generated yet."))
                    color: tools.editor.imageCompletionHasCandidate
                        ? Theme.textPrimary : Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.Wrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !tools.editor.imageCompletionHasCandidate
            spacing: 8

            ShadowButton {
                objectName: "imageCompletionGenerateButton"
                Layout.fillWidth: true
                variant: tools.editor.imageCompletionBusy
                    ? ShadowButton.Secondary : ShadowButton.Primary
                text: tools.editor.imageCompletionBusy
                    ? qsTr("Cancel generation") : qsTr("Generate preview")
                enabled: tools.editor.imageCompletionBusy
                    || tools.editor.imageCompletionCanGenerate
                onClicked: {
                    if (tools.editor.imageCompletionBusy) {
                        tools.editor.cancelImageCompletion()
                        tools.exitRequested()
                    } else {
                        tools.requestGeneration()
                    }
                }
            }

            ShadowIconButton {
                objectName: "imageCompletionExitButton"
                buttonSize: 32
                source: "qrc:/icons/close.svg"
                toolTipText: qsTr("Exit")
                enabled: !tools.editor.imageCompletionBusy
                onClicked: tools.exitRequested()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: tools.editor.imageCompletionHasCandidate
            spacing: 8

            ShadowButton {
                objectName: "imageCompletionApplyButton"
                Layout.fillWidth: true
                variant: ShadowButton.Primary
                text: qsTr("Apply")
                onClicked: tools.editor.applyImageCompletionCandidate()
            }
            ShadowIconButton {
                objectName: "imageCompletionRetryButton"
                buttonSize: 32
                source: "qrc:/icons/refresh.svg"
                toolTipText: qsTr("Retry")
                onClicked: tools.editor.retryImageCompletion()
            }
            ShadowIconButton {
                objectName: "imageCompletionCancelButton"
                buttonSize: 32
                source: "qrc:/icons/close.svg"
                toolTipText: qsTr("Cancel")
                onClicked: tools.exitRequested()
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        height: 1
        visible: tools.editor.imageCompletionRegions.length > 0
        color: Theme.border
    }

    Label {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: tools.editor.imageCompletionRegions.length > 0
        text: qsTr("ACCEPTED REGIONS · %L1")
            .arg(tools.editor.imageCompletionRegions.length)
        color: Theme.textSecondary
        font.pixelSize: Theme.fontMeta
        font.weight: Font.DemiBold
        wrapMode: Text.Wrap
    }

    Repeater {
        // Keep delegates stable when a strength edit republishes the region
        // list. Replacing the model by value would destroy an active slider.
        model: tools.authoring ? 0 : tools.editor.imageCompletionRegions.length

        delegate: Rectangle {
            id: regionCard
            required property int index
            readonly property var region: tools.editor.imageCompletionRegions[index] || ({})

            Layout.fillWidth: true
            implicitHeight: regionContent.implicitHeight + 20
            radius: Theme.controlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.border

            ColumnLayout {
                id: regionContent
                anchors.fill: parent
                anchors.margins: 10
                spacing: 7

                RowLayout {
                    Layout.fillWidth: true

                    ShadowSwitch {
                        objectName: "imageCompletionRegionEnabled_" + regionCard.index
                        Layout.fillWidth: true
                        text: qsTr("Region %L1").arg(regionCard.index + 1)
                        checked: Boolean(regionCard.region.enabled)
                        enabled: !tools.authoring && !tools.editor.stateBusy
                        onToggled: tools.editor.setImageCompletionRegionEnabled(
                            regionCard.index, checked)
                    }

                    ShadowIconButton {
                        objectName: "imageCompletionRegionRefresh_" + regionCard.index
                        source: "qrc:/icons/refresh.svg"
                        toolTipText: qsTr("Regenerate this region with the current photo")
                        accessibleName: toolTipText
                        enabled: !tools.authoring && !tools.editor.stateBusy
                        onClicked: tools.refreshRequested(regionCard.index)
                    }

                    ShadowIconButton {
                        objectName: "imageCompletionRegionRemove_" + regionCard.index
                        source: "qrc:/icons/clear.svg"
                        toolTipText: qsTr("Remove this accepted region")
                        accessibleName: toolTipText
                        enabled: !tools.authoring && !tools.editor.stateBusy
                        onClicked: tools.editor.removeImageCompletionRegion(
                            regionCard.index)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: !Boolean(regionCard.region.preGrade)
                    text: qsTr("Older region: regenerate it to follow future adjustments")
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                }

                ShadowSlider {
                    objectName: "imageCompletionRegionStrength_" + regionCard.index
                    readonly property string parameterKey:
                        "image_completion/region/" + regionCard.index + "/strength"
                    Layout.fillWidth: true
                    label: qsTr("Strength")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    neutralValue: 1
                    decimals: 0
                    displayMultiplier: 100
                    suffix: "%"
                    value: Number(regionCard.region.strength)
                    enabled: !tools.authoring && !tools.editor.stateBusy
                    onGestureStarted: tools.editor.beginParameterEdit(parameterKey)
                    onEdited: value =>
                        tools.editor.setImageCompletionRegionStrength(
                            regionCard.index, value)
                    onGestureFinished: tools.editor.endParameterEdit(parameterKey)
                }

                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: qsTr("%1 · %2")
                        .arg(String(regionCard.region.provider))
                        .arg(String(regionCard.region.modelBuild))
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    elide: Text.ElideMiddle
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: tools.authoring && tools.editor.imageCompletionRegions.length > 0
        text: qsTr("Finish painting to adjust the accepted regions.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.Wrap
    }
}
