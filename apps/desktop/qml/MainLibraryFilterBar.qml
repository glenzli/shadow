pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Owns the complete Library filter projection and its controller mutations.
Rectangle {
    id: filterBar

    required property var controller
    required property var semanticSearchController
    signal advancedFilterRequested()

    readonly property bool anyFilterActive:
        filterBar.controller.filterHideOfflineUncached === true
        || filterBar.controller.filterOnlyEditable === true
        || filterBar.controller.filterFlag !== "all"
        || filterBar.controller.filterMinimumRating > 0
        || filterBar.controller.filterColorLabel !== "all"
        || filterBar.controller.filterEditState !== "all"
        || filterBar.controller.filterLiked !== "all"
        || filterBar.controller.filterExcludedFlag !== "all"
        || filterBar.controller.filterExcludedColorLabel !== "all"
        || filterBar.controller.filterCaptureMonth.length > 0
        || filterBar.controller.filterChineseLunarMonth > 0
        || filterBar.controller.filterChineseLunarDay > 0
        || filterBar.controller.filterChineseLunarMonthType !== "all"
        || filterBar.controller.filterCameraKey.length > 0
        || filterBar.controller.filterLensKey.length > 0
        || filterBar.controller.filterKeywordIdsAll.length > 0
        || filterBar.controller.filterExcludedKeywordIdsAny.length > 0
        || filterBar.semanticSearchController.hasResults

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

    function flagToolTip(flag) {
        switch (String(flag).toLowerCase()) {
        case "unflagged":
            return qsTranslate("Main", "Filter unflagged photos")
        case "picked":
            return qsTranslate("Main", "Filter flagged photos")
        case "rejected":
            return qsTranslate("Main", "Filter rejected photos")
        default:
            return qsTranslate("Main", "Clear all Library filters")
        }
    }

    implicitWidth: filterControls.implicitWidth + 12
    implicitHeight: Theme.controlHeight
    radius: 7
    color: filterBar.anyFilterActive
        ? Theme.accentSurfaceQuiet : Theme.surfaceSubtle

    MainLibrarySortMenu {
        id: sortMenu
        controller: filterBar.controller
    }

    Row {
        id: filterControls
        anchors.centerIn: parent
        spacing: 2

        ShadowButton {
            id: sortButton
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            minimumButtonWidth: 64
            variant: ShadowButton.Ghost
            text: filterBar.controller.librarySortKey === "name"
                ? (filterBar.controller.librarySortDescending
                    ? qsTranslate("Main", "NAME ↓")
                    : qsTranslate("Main", "NAME ↑"))
                : (filterBar.controller.librarySortDescending
                    ? qsTranslate("Main", "DATE ↓")
                    : qsTranslate("Main", "DATE ↑"))
            toolTipText: qsTranslate("Main", "Sort Library photos")
            onClicked: sortMenu.presentFrom(sortButton)
        }

        Rectangle {
            width: 1
            height: 16
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.border
        }

        ShadowIconButton {
            anchors.verticalCenter: parent.verticalCenter
            source: "qrc:/icons/filter.svg"
            buttonSize: 24
            iconSize: 14
            variant: ShadowIconButton.Ghost
            selected: filterBar.controller.filterHideOfflineUncached
                || filterBar.controller.filterExcludedFlag !== "all"
                || filterBar.controller.filterExcludedColorLabel !== "all"
                || filterBar.controller.filterChineseLunarMonth > 0
                || filterBar.controller.filterChineseLunarDay > 0
                || filterBar.controller.filterChineseLunarMonthType !== "all"
                || filterBar.controller.filterKeywordIdsAll.length > 0
                || filterBar.controller.filterExcludedKeywordIdsAny.length > 0
            toolTipText: qsTranslate("Main", "Open advanced Library filters")
            accessibleName: toolTipText
            onClicked: filterBar.advancedFilterRequested()
        }

        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTranslate("Main", "FILTER")
            color: Theme.textMuted
            font.pixelSize: Theme.fontCaption
            font.letterSpacing: 0.7

            TapHandler {
                onTapped: filterBar.advancedFilterRequested()
            }
        }

        ShadowIconButton {
            anchors.verticalCenter: parent.verticalCenter
            buttonSize: 24
            iconSize: 14
            source: "qrc:/icons/filter-off.svg"
            selected: !filterBar.anyFilterActive
            enabled: !filterBar.semanticSearchController.busy
            toolTipText: qsTranslate("Main", "Clear all Library filters")
            accessibleName: toolTipText
            onClicked: filterBar.controller.clearFilters()
        }

        ShadowIconButton {
            objectName: "originalAvailableButton"
            anchors.verticalCenter: parent.verticalCenter
            source: "qrc:/icons/source-available.svg"
            buttonSize: 24
            iconSize: 17
            checkable: true
            checked: filterBar.controller.filterOnlyEditable
            accessibleName: qsTr("Original available")
            toolTipText: qsTr("Show only photos whose original is available locally or can be retrieved online")
            onToggled: filterBar.controller.filterOnlyEditable = checked
        }

        Repeater {
            model: ListModel {
                ListElement {
                    filterValue: "unflagged"
                    iconSource: "qrc:/icons/unflag.svg"
                }
                ListElement {
                    filterValue: "picked"
                    iconSource: "qrc:/icons/pick.svg"
                }
                ListElement {
                    filterValue: "rejected"
                    iconSource: "qrc:/icons/reject.svg"
                }
            }

            delegate: ShadowIconButton {
                required property string filterValue
                required property url iconSource
                anchors.verticalCenter: parent.verticalCenter
                buttonSize: 24
                iconSize: 15
                source: iconSource
                selected: filterBar.controller.filterFlag === filterValue
                toolTipText: filterBar.flagToolTip(filterValue)
                accessibleName: toolTipText
                onClicked: filterBar.controller.filterFlag = selected
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
                anchors.verticalCenter: parent.verticalCenter
                buttonSize: 24
                iconSize: 15
                source: "qrc:/icons/star-filled.svg"
                selected: filterBar.controller.filterMinimumRating === index + 1
                foregroundColor:
                    filterBar.controller.filterMinimumRating >= index + 1
                    ? Theme.labelYellow : Theme.textMuted
                toolTipText: qsTranslate(
                    "Main", "Filter %L1 stars and above").arg(index + 1)
                accessibleName: toolTipText
                onClicked: filterBar.controller.filterMinimumRating =
                    selected ? 0 : index + 1
            }
        }

        Rectangle {
            width: 1
            height: 16
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.border
        }

        ShadowIconButton {
            anchors.verticalCenter: parent.verticalCenter
            buttonSize: 24
            iconSize: 15
            source: "qrc:/icons/heart-filled.svg"
            selected: filterBar.controller.filterLiked === "liked"
            foregroundColor: Theme.likeAccent
            selectedSurfaceColor: Theme.likeSurface
            selectedHoverSurfaceColor: Theme.likeHoverSurface
            selectedPressedSurfaceColor: Theme.likePressedSurface
            selectedIconColor: Theme.likeAccent
            toolTipText: qsTranslate("Main", "Filter liked photos")
            accessibleName: toolTipText
            onClicked: filterBar.controller.filterLiked =
                selected ? "all" : "liked"
        }

        Rectangle {
            width: 1
            height: 16
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.border
        }

        ShadowIconButton {
            anchors.verticalCenter: parent.verticalCenter
            buttonSize: 24
            iconSize: 15
            source: "qrc:/icons/edit.svg"
            selected: filterBar.controller.filterEditState === "edited"
            toolTipText: qsTranslate("Main", "Filter edited photos")
            accessibleName: toolTipText
            onClicked: filterBar.controller.filterEditState =
                selected ? "all" : "edited"
        }

        ShadowIconButton {
            anchors.verticalCenter: parent.verticalCenter
            buttonSize: 24
            iconSize: 15
            source: "qrc:/icons/edit-off.svg"
            selected: filterBar.controller.filterEditState === "unedited"
            toolTipText: qsTranslate("Main", "Filter unedited photos")
            accessibleName: toolTipText
            onClicked: filterBar.controller.filterEditState =
                selected ? "all" : "unedited"
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
                anchors.verticalCenter: parent.verticalCenter
                labelColor: Theme.colorLabel(modelData)
                selected:
                    filterBar.controller.filterColorLabel === modelData
                toolTipText: qsTranslate(
                    "Main", "Filter %1 color label").arg(
                        filterBar.colorLabelName(modelData))
                accessibleName: toolTipText
                onClicked: filterBar.controller.filterColorLabel =
                    selected ? "all" : modelData
            }
        }
    }
}
