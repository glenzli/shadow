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
    signal exitRequested

    spacing: 10

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
        title: qsTr("Allow local AI Completion?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: {
            tools.editor.imageCompletionExecutionAllowed = true
            tools.editor.generateImageCompletion()
        }

        Label {
            width: Math.min(440, localExecutionDialog.width - 48)
            text: qsTr("Shadow will send a bounded crop of this photo and your painted selection to Infer Runtime on this device. The request is not uploaded by Shadow. You can turn this permission off later in AI & Models settings.")
            color: Theme.textPrimary
            wrapMode: Text.WordWrap
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("AI COMPLETION")
        color: Theme.textPrimary
        font.pixelSize: 12
        font.weight: Font.DemiBold
        font.letterSpacing: 0.25
    }

    Label {
        Layout.fillWidth: true
        text: tools.authoring
            ? qsTr("Paint the area to replace. Generation creates a preview candidate; the photo Recipe changes only after Apply.")
            : qsTr("Accepted regions belong to this photo's fixed AI Completion node and are used by preview, detail, and export.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.WordWrap
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

            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("Paint")
                checkable: true
                checked: !tools.editor.imageCompletionEraseMode
                selected: checked
                enabled: !tools.editor.imageCompletionBusy
                onClicked: tools.editor.imageCompletionEraseMode = false
            }

            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("Erase")
                checkable: true
                checked: tools.editor.imageCompletionEraseMode
                selected: checked
                enabled: !tools.editor.imageCompletionBusy
                onClicked: tools.editor.imageCompletionEraseMode = true
            }
        }

        ShadowSlider {
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
            onEdited: value => tools.editor.imageCompletionBrushRadius = value
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("Undo stroke")
                enabled: tools.editor.imageCompletionBrushPoints.length > 0
                    && !tools.editor.imageCompletionBusy
                onClicked: tools.editor.undoImageCompletionStroke()
            }

            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("Clear")
                enabled: tools.editor.imageCompletionBrushPoints.length > 0
                    && !tools.editor.imageCompletionBusy
                onClicked: tools.editor.clearImageCompletionSelection()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: tools.editor.liquifyNodeMaterialized
                && tools.editor.liquifyNodeEnabled
            text: qsTr("AI Completion is evaluated before Liquify. Bypass the Liquify node while painting and generating, then turn it back on.")
            color: Theme.warningText
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: candidateStatus.implicitHeight + 20
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
                    text: tools.editor.imageCompletionBusy
                        ? qsTr("Generating a local candidate…")
                        : (tools.editor.imageCompletionHasCandidate
                            ? qsTr("Candidate preview is shown on the photo.")
                            : qsTr("No candidate generated yet."))
                    color: tools.editor.imageCompletionHasCandidate
                        ? Theme.textPrimary : Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !tools.editor.imageCompletionHasCandidate
            spacing: 8

            ShadowButton {
                Layout.fillWidth: true
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

            ShadowButton {
                text: qsTr("Exit")
                enabled: !tools.editor.imageCompletionBusy
                onClicked: tools.exitRequested()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: tools.editor.imageCompletionHasCandidate
            spacing: 8

            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("Apply")
                onClicked: tools.editor.applyImageCompletionCandidate()
            }
            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("Retry")
                onClicked: tools.editor.retryImageCompletion()
            }
            ShadowButton {
                text: qsTr("Cancel")
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
        visible: tools.editor.imageCompletionRegions.length > 0
        text: qsTr("ACCEPTED REGIONS · %L1")
            .arg(tools.editor.imageCompletionRegions.length)
        color: Theme.textSecondary
        font.pixelSize: 10
        font.weight: Font.DemiBold
    }

    Repeater {
        model: tools.editor.imageCompletionRegions

        delegate: Rectangle {
            id: regionCard
            required property int index
            required property var modelData

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
                        Layout.fillWidth: true
                        text: qsTr("Region %L1").arg(regionCard.index + 1)
                        checked: Boolean(regionCard.modelData.enabled)
                        enabled: !tools.authoring && !tools.editor.stateBusy
                        onToggled: tools.editor.setImageCompletionRegionEnabled(
                            regionCard.index, checked)
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/clear.svg"
                        toolTipText: qsTr("Remove this accepted region")
                        accessibleName: toolTipText
                        enabled: !tools.authoring && !tools.editor.stateBusy
                        onClicked: tools.editor.removeImageCompletionRegion(
                            regionCard.index)
                    }
                }

                ShadowSlider {
                    Layout.fillWidth: true
                    label: qsTr("Strength")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    neutralValue: 1
                    decimals: 0
                    displayMultiplier: 100
                    suffix: "%"
                    value: Number(regionCard.modelData.strength)
                    enabled: !tools.authoring && !tools.editor.stateBusy
                    onEdited: value =>
                        tools.editor.setImageCompletionRegionStrength(
                            regionCard.index, value)
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 · %2")
                        .arg(String(regionCard.modelData.provider))
                        .arg(String(regionCard.modelData.modelBuild))
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    elide: Text.ElideMiddle
                }
            }
        }
    }
}
