pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Owns the selected-photo decision transaction: undo, flag, rating, and color.
Rectangle {
    id: decisionBar

    required property var controller
    required property var reviewWorkspace

    function colorLabelName(label) {
        switch (String(label).toLowerCase()) {
        case "red":
            return qsTranslate("Main", "Red")
        case "yellow":
            return qsTranslate("Main", "Yellow")
        case "green":
            return qsTranslate("Main", "Green")
        case "blue":
            return qsTranslate("Main", "Blue")
        case "purple":
            return qsTranslate("Main", "Purple")
        default:
            return ""
        }
    }

    implicitWidth: selectionControls.implicitWidth + 12
    implicitHeight: 28
    radius: 7
    color: decisionBar.reviewWorkspace.canMutateDecision
        ? Theme.accentSurfaceQuiet : Theme.buttonDisabledGhostSurface

    Row {
        id: selectionControls
        anchors.centerIn: parent
        spacing: 2

        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: decisionBar.reviewWorkspace.canMutateDecision
                ? qsTranslate("Main", "SELECTED")
                : qsTranslate("Main", "NO SELECTION")
            color: decisionBar.reviewWorkspace.canMutateDecision
                ? Theme.accentTextMuted : Theme.textDisabledQuiet
            font.pixelSize: 9
            font.letterSpacing: 0.7
        }

        Row {
            visible: decisionBar.reviewWorkspace.canMutateDecision
            spacing: 2

            ShadowIconButton {
                buttonSize: 24
                iconSize: 14
                source: "qrc:/icons/undo.svg"
                toolTipText: qsTranslate("Main", "Undo the last decision")
                accessibleName: toolTipText
                enabled: decisionBar.controller.canUndoDecision
                onClicked: decisionBar.controller.undoLastDecision()
            }

            Rectangle {
                width: 1
                height: 16
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.border
            }

            Repeater {
                model: ListModel {
                    ListElement {
                        flagValue: "unflagged"
                        iconSource: "qrc:/icons/unflag.svg"
                    }
                    ListElement {
                        flagValue: "picked"
                        iconSource: "qrc:/icons/pick.svg"
                    }
                    ListElement {
                        flagValue: "rejected"
                        iconSource: "qrc:/icons/reject.svg"
                    }
                }

                delegate: ShadowIconButton {
                    required property string flagValue
                    required property url iconSource
                    buttonSize: 24
                    iconSize: 15
                    source: iconSource
                    toolTipText: flagValue === "picked"
                        ? qsTranslate("Main", "Mark as picked (P)")
                        : flagValue === "rejected"
                            ? qsTranslate("Main", "Mark as rejected (X)")
                            : qsTranslate("Main", "Clear decision flag (U)")
                    accessibleName: toolTipText
                    selected:
                        decisionBar.reviewWorkspace.selectedDecisionFlag
                            === flagValue
                    selectedSurfaceColor: flagValue === "picked"
                        ? Theme.successSurface
                        : flagValue === "rejected"
                            ? Theme.dangerSurface : Theme.accentSurface
                    selectedHoverSurfaceColor: selectedSurfaceColor
                    selectedPressedSurfaceColor: selectedSurfaceColor
                    selectedIconColor: flagValue === "picked"
                        ? Theme.successText
                        : flagValue === "rejected"
                            ? Theme.dangerText : Theme.accent
                    onClicked:
                        decisionBar.reviewWorkspace.setSelectedFlag(flagValue)
                }
            }

            Rectangle {
                width: 1
                height: 16
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.border
            }

            ShadowIconButton {
                buttonSize: 24
                iconSize: 13
                source: "qrc:/icons/clear.svg"
                selected:
                    decisionBar.reviewWorkspace.selectedDecisionRating === 0
                toolTipText: qsTranslate("Main", "Clear rating (0)")
                accessibleName: toolTipText
                onClicked:
                    decisionBar.reviewWorkspace.setSelectedRating(0)
            }

            Repeater {
                model: 5

                delegate: ShadowIconButton {
                    required property int index
                    buttonSize: 24
                    iconSize: 15
                    source:
                        index
                            < decisionBar.reviewWorkspace.selectedDecisionRating
                        ? "qrc:/icons/star-filled.svg"
                        : "qrc:/icons/star.svg"
                    foregroundColor:
                        index
                            < decisionBar.reviewWorkspace.selectedDecisionRating
                        ? Theme.labelYellow : Theme.textMuted
                    toolTipText: qsTranslate(
                        "Main", "Set rating to %L1 stars (%L1)").arg(index + 1)
                    accessibleName: toolTipText
                    onClicked:
                        decisionBar.reviewWorkspace.setSelectedRating(index + 1)
                }
            }

            Rectangle {
                width: 1
                height: 16
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.border
            }

            ShadowIconButton {
                buttonSize: 24
                iconSize: 13
                source: "qrc:/icons/clear.svg"
                selected:
                    decisionBar.reviewWorkspace.selectedColorLabel === "none"
                toolTipText: qsTranslate("Main", "Clear local color label")
                accessibleName: toolTipText
                onClicked: decisionBar.controller.setPhotoColorLabel(
                    decisionBar.reviewWorkspace.selectedPhotoId, "none")
            }

            Repeater {
                model: ["red", "yellow", "green", "blue", "purple"]

                delegate: ShadowColorLabelButton {
                    required property string modelData
                    labelColor: Theme.colorLabel(modelData)
                    selected:
                        decisionBar.reviewWorkspace.selectedColorLabel
                            === modelData
                    toolTipText: qsTranslate(
                        "Main", "Set %1 color label").arg(
                            decisionBar.colorLabelName(modelData))
                    accessibleName: toolTipText
                    onClicked: decisionBar.controller.setPhotoColorLabel(
                        decisionBar.reviewWorkspace.selectedPhotoId, modelData)
                }
            }
        }
    }
}
