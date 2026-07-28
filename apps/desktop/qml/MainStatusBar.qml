pragma ComponentBehavior: Bound
pragma Translator: "Main"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: statusBar

    required property int workspaceIndex
    required property var controller
    required property var editor
    required property var reviewWorkspace
    required property var precisionWorkspace

    function colorLabelName(label) {
        switch (String(label).toLowerCase()) {
        case "red": return qsTr("Red")
        case "yellow": return qsTr("Yellow")
        case "green": return qsTr("Green")
        case "blue": return qsTr("Blue")
        case "purple": return qsTr("Purple")
        default: return ""
        }
    }

    function filterFlagToolTip(flag) {
        switch (String(flag).toLowerCase()) {
        case "unflagged": return qsTr("Filter unflagged photos")
        case "picked": return qsTr("Filter flagged photos")
        case "rejected": return qsTr("Filter rejected photos")
        default: return qsTr("Clear all Library filters")
        }
    }

    function fullResolutionPreparationText() {
        const path = String(editor.sourcePath).toLowerCase()
        return /\.(jpe?g|heic|heif)$/.test(path)
            ? qsTr("Loading full-resolution image…")
            : qsTr("Parsing full-resolution RAW…")
    }

    height: statusBar.workspaceIndex === 0 ? 40 : 30
    color: Theme.chrome
    border.color: Theme.border

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 7

        BusyIndicator {
            Layout.preferredWidth: 15
            Layout.preferredHeight: 15
            visible: statusBar.workspaceIndex !== 1
                ? statusBar.controller.scanning || statusBar.controller.refreshing
                    || statusBar.controller.busy
                    || statusBar.controller.loadingMore
                    || statusBar.controller.comparisonBusy
                    || statusBar.controller.decisionBusy
                : statusBar.editor.busy
                    || statusBar.editor.fullResolutionPreparing
            running: visible
        }

        Rectangle {
            visible: statusBar.workspaceIndex === 0
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: 28
            implicitWidth: filterControls.implicitWidth + 12
            radius: 7
            color: statusBar.controller.filterFlag !== "all"
                || statusBar.controller.filterMinimumRating > 0
                || statusBar.controller.filterColorLabel !== "all"
                || statusBar.controller.filterEditState !== "all"
                || statusBar.controller.filterLiked !== "all"
                ? Theme.accentSurfaceQuiet : Theme.surfaceSubtle

            Row {
                id: filterControls
                anchors.centerIn: parent
                spacing: 2

                ShadowIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/icons/filter.svg"
                    size: 14
                    color: Theme.textMuted
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("FILTER")
                    color: Theme.textMuted
                    font.pixelSize: 9
                    font.letterSpacing: 0.7
                }

                ShadowIconButton {
                    buttonSize: 24
                    iconSize: 14
                    source: "qrc:/icons/clear.svg"
                    selected: statusBar.controller.filterFlag === "all"
                        && statusBar.controller.filterMinimumRating === 0
                        && statusBar.controller.filterColorLabel === "all"
                        && statusBar.controller.filterEditState === "all"
                        && statusBar.controller.filterLiked === "all"
                    toolTipText: qsTr("Clear all Library filters")
                    accessibleName: toolTipText
                    onClicked: statusBar.controller.clearFilters()
                }

                Repeater {
                    model: ListModel {
                        ListElement { filterValue: "unflagged"; iconSource: "qrc:/icons/unflag.svg" }
                        ListElement { filterValue: "picked"; iconSource: "qrc:/icons/pick.svg" }
                        ListElement { filterValue: "rejected"; iconSource: "qrc:/icons/reject.svg" }
                    }

                    delegate: ShadowIconButton {
                        required property string filterValue
                        required property url iconSource
                        buttonSize: 24
                        iconSize: 15
                        source: iconSource
                        selected: statusBar.controller.filterFlag === filterValue
                        toolTipText: statusBar.filterFlagToolTip(filterValue)
                        accessibleName: toolTipText
                        onClicked: statusBar.controller.filterFlag = selected
                            ? "all" : filterValue
                    }
                }

                Rectangle {
                    width: 1
                    height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.border
                }

                Repeater {
                    model: 5

                    delegate: ShadowIconButton {
                        required property int index
                        buttonSize: 24
                        iconSize: 15
                        source: "qrc:/icons/star-filled.svg"
                        selected: statusBar.controller.filterMinimumRating
                            === index + 1
                        foregroundColor: statusBar.controller.filterMinimumRating
                            >= index + 1 ? Theme.labelYellow : Theme.textMuted
                        toolTipText: qsTr("Filter %L1 stars and above")
                            .arg(index + 1)
                        accessibleName: toolTipText
                        onClicked: statusBar.controller.filterMinimumRating
                            = selected ? 0 : index + 1
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
                    iconSize: 15
                    source: "qrc:/icons/heart-filled.svg"
                    selected: statusBar.controller.filterLiked === "liked"
                    foregroundColor: selected ? Theme.accent : Theme.textMuted
                    toolTipText: qsTr("Filter liked photos")
                    accessibleName: toolTipText
                    onClicked: statusBar.controller.filterLiked = selected
                        ? "all" : "liked"
                }

                Rectangle {
                    width: 1
                    height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.border
                }

                ShadowIconButton {
                    buttonSize: 24
                    iconSize: 15
                    source: "qrc:/icons/edit.svg"
                    selected: statusBar.controller.filterEditState === "edited"
                    toolTipText: qsTr("Filter edited photos")
                    accessibleName: toolTipText
                    onClicked: statusBar.controller.filterEditState = selected
                        ? "all" : "edited"
                }

                ShadowIconButton {
                    buttonSize: 24
                    iconSize: 15
                    source: "qrc:/icons/edit-off.svg"
                    selected: statusBar.controller.filterEditState === "unedited"
                    toolTipText: qsTr("Filter unedited photos")
                    accessibleName: toolTipText
                    onClicked: statusBar.controller.filterEditState = selected
                        ? "all" : "unedited"
                }

                Rectangle {
                    width: 1
                    height: 16
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.border
                }

                Repeater {
                    model: ["red", "yellow", "green", "blue", "purple"]

                    delegate: ShadowColorLabelButton {
                        required property string modelData
                        labelColor: Theme.colorLabel(modelData)
                        selected: statusBar.controller.filterColorLabel === modelData
                        toolTipText: qsTr("Filter %1 color label").arg(
                            statusBar.colorLabelName(modelData)
                        )
                        accessibleName: toolTipText
                        onClicked: statusBar.controller.filterColorLabel = selected
                            ? "all" : modelData
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: statusBar.workspaceIndex === 0
                ? qsTr("%L1 / %L2 photos").arg(
                    statusBar.controller.filteredItemCount
                ).arg(statusBar.controller.itemCount)
                : statusBar.workspaceIndex !== 1
                ? (statusBar.controller.decisionBusy
                    ? statusBar.controller.decisionStatusText
                    : statusBar.controller.comparisonBusy
                    ? statusBar.controller.comparisonStatusText
                    : statusBar.controller.statusText)
                : statusBar.editor.fullResolutionPreparing
                    ? statusBar.fullResolutionPreparationText()
                    : statusBar.editor.statusText
            color: Theme.textMuted
            font.pixelSize: 10
            elide: Text.ElideRight
        }

        Rectangle {
            visible: statusBar.workspaceIndex === 1 && statusBar.editor.active
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 1
            Layout.preferredHeight: 16
            color: Theme.border
        }

        RowLayout {
            visible: statusBar.workspaceIndex === 1 && statusBar.editor.active
            Layout.alignment: Qt.AlignVCenter
            spacing: 5

            Label {
                text: qsTr("PROXY")
                color: Theme.textMuted
                font.pixelSize: 9
                font.letterSpacing: 0.7
            }

            Rectangle {
                Layout.preferredWidth: 6
                Layout.preferredHeight: 6
                radius: 3
                color: statusBar.precisionWorkspace.proxyActive
                    ? Theme.accent : Theme.textMuted
            }

            Label {
                text: statusBar.precisionWorkspace.proxyActive
                    ? qsTr("ON") : qsTr("OFF")
                color: statusBar.precisionWorkspace.proxyActive
                    ? Theme.accent : Theme.textMuted
                font.pixelSize: 9
                font.weight: Font.DemiBold
            }
        }

        Rectangle {
            visible: statusBar.workspaceIndex === 0
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: 28
            implicitWidth: selectionControls.implicitWidth + 12
            radius: 7
            color: statusBar.reviewWorkspace.canMutateDecision
                ? Theme.accentSurfaceQuiet : Theme.buttonDisabledGhostSurface

            Row {
                id: selectionControls
                anchors.centerIn: parent
                spacing: 2

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: statusBar.reviewWorkspace.canMutateDecision
                        ? qsTr("SELECTED") : qsTr("NO SELECTION")
                    color: statusBar.reviewWorkspace.canMutateDecision
                        ? Theme.accentTextMuted : Theme.textDisabledQuiet
                    font.pixelSize: 9
                    font.letterSpacing: 0.7
                }

                Row {
                    visible: statusBar.reviewWorkspace.canMutateDecision
                    spacing: 2

                    ShadowIconButton {
                        buttonSize: 24
                        iconSize: 14
                        source: "qrc:/icons/undo.svg"
                        toolTipText: qsTr("Undo the last decision")
                        accessibleName: toolTipText
                        enabled: statusBar.controller.canUndoDecision
                        onClicked: statusBar.controller.undoLastDecision()
                    }

                    Rectangle {
                        width: 1
                        height: 16
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.border
                    }

                    Repeater {
                        model: ListModel {
                            ListElement { actionId: "none"; flagValue: "unflagged"; iconSource: "qrc:/icons/unflag.svg" }
                            ListElement { actionId: "pick"; flagValue: "picked"; iconSource: "qrc:/icons/pick.svg" }
                            ListElement { actionId: "reject"; flagValue: "rejected"; iconSource: "qrc:/icons/reject.svg" }
                        }

                        delegate: ShadowIconButton {
                            required property string flagValue
                            required property url iconSource
                            buttonSize: 24
                            iconSize: 15
                            source: iconSource
                            toolTipText: flagValue === "picked"
                                ? qsTr("Mark as picked (P)")
                                : flagValue === "rejected"
                                    ? qsTr("Mark as rejected (X)")
                                    : qsTr("Clear decision flag (U)")
                            accessibleName: toolTipText
                            selected: statusBar.reviewWorkspace.selectedDecisionFlag === flagValue
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
                            onClicked: statusBar.reviewWorkspace.setSelectedFlag(flagValue)
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
                        selected: statusBar.reviewWorkspace.selectedDecisionRating === 0
                        toolTipText: qsTr("Clear rating (0)")
                        accessibleName: toolTipText
                        onClicked: statusBar.reviewWorkspace.setSelectedRating(0)
                    }

                    Repeater {
                        model: 5

                        delegate: ShadowIconButton {
                            required property int index
                            buttonSize: 24
                            iconSize: 15
                            source: index < statusBar.reviewWorkspace.selectedDecisionRating
                                ? "qrc:/icons/star-filled.svg" : "qrc:/icons/star.svg"
                            foregroundColor: index < statusBar.reviewWorkspace.selectedDecisionRating
                                ? Theme.labelYellow : Theme.textMuted
                            toolTipText: qsTr("Set rating to %L1 stars (%L1)")
                                .arg(index + 1)
                            accessibleName: toolTipText
                            onClicked: statusBar.reviewWorkspace.setSelectedRating(index + 1)
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
                        selected: statusBar.reviewWorkspace.selectedColorLabel === "none"
                        toolTipText: qsTr("Clear local color label")
                        accessibleName: toolTipText
                        onClicked: statusBar.controller.setPhotoColorLabel(
                            statusBar.reviewWorkspace.selectedPhotoId, "none")
                    }

                    Repeater {
                        model: ["red", "yellow", "green", "blue", "purple"]

                        delegate: ShadowColorLabelButton {
                            required property string modelData
                            labelColor: Theme.colorLabel(modelData)
                            selected: statusBar.reviewWorkspace.selectedColorLabel === modelData
                            toolTipText: qsTr("Set %1 color label").arg(
                                statusBar.colorLabelName(modelData)
                            )
                            accessibleName: toolTipText
                            onClicked: statusBar.controller.setPhotoColorLabel(
                                statusBar.reviewWorkspace.selectedPhotoId, modelData)
                        }
                    }
                }
            }
        }

        Label {
            visible: statusBar.workspaceIndex !== 0
            text: statusBar.workspaceIndex === 1
                ? qsTr("PRECISION")
                : qsTr("LIBRARY")
            color: Theme.textFaint
            font.pixelSize: 9
            font.letterSpacing: 0.8
        }
    }

}
