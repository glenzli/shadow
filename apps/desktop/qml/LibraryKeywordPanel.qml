pragma ComponentBehavior: Bound
pragma Translator: "LibraryKeywords"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Reusable keyword surface. Library Management enables taxonomy mutation;
// Review uses the same tree for batch assignment and typed include/exclude
// predicates, so organization and retrieval cannot silently diverge.
Item {
    id: root

    required property var controller
    property var targets: []
    property string primaryPhotoId: ""
    property bool allowAssignment: false
    property bool allowFiltering: true
    property bool manageTaxonomy: false
    property bool allowCreation: manageTaxonomy
    property bool showHeading: true
    property string searchText: ""

    implicitHeight: 430
    implicitWidth: 520

    readonly property var visibleKeywords: {
        const query = searchText.trim().toLocaleLowerCase()
        const keywords = controller.libraryKeywords
        if (query.length === 0)
            return keywords
        const result = []
        for (let index = 0; index < keywords.length; ++index) {
            if (String(keywords[index].name).toLocaleLowerCase()
                    .includes(query)) {
                result.push(keywords[index])
            }
        }
        return result
    }

    function containsId(values, id) {
        for (let index = 0; index < values.length; ++index) {
            if (String(values[index]) === String(id))
                return true
        }
        return false
    }

    function photoHasKeyword(id) {
        const assignments = controller.libraryPhotoKeywords
        for (let index = 0; index < assignments.length; ++index) {
            if (String(assignments[index].id) === String(id))
                return true
        }
        return false
    }

    function toggledIds(values, id, enabled) {
        const result = []
        for (let index = 0; index < values.length; ++index) {
            if (String(values[index]) !== String(id))
                result.push(String(values[index]))
        }
        if (enabled)
            result.push(String(id))
        return result
    }

    function toggleRequired(id) {
        const active = containsId(controller.filterKeywordIdsAll, id)
        controller.filterKeywordIdsAll = toggledIds(
            controller.filterKeywordIdsAll, id, !active)
        if (!active) {
            controller.filterExcludedKeywordIdsAny = toggledIds(
                controller.filterExcludedKeywordIdsAny, id, false)
        }
    }

    function toggleExcluded(id) {
        const active = containsId(
            controller.filterExcludedKeywordIdsAny, id)
        controller.filterExcludedKeywordIdsAny = toggledIds(
            controller.filterExcludedKeywordIdsAny, id, !active)
        if (!active) {
            controller.filterKeywordIdsAll = toggledIds(
                controller.filterKeywordIdsAll, id, false)
        }
    }

    Component.onCompleted: controller.refreshLibraryKeywords()

    onPrimaryPhotoIdChanged:
        controller.requestLibraryKeywordsForPhoto(primaryPhotoId)

    LibraryKeywordDialogs {
        id: dialogs
        anchors.fill: parent
        controller: root.controller
        hostWidth: root.width
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            visible: root.showHeading
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: qsTr("Keywords")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSection
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("A parent matches assignments anywhere in its subtree.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }
            }

            ShadowButton {
                visible: root.allowCreation
                compact: true
                variant: ShadowButton.Ghost
                text: qsTr("New keyword")
                enabled: !root.controller.libraryKeywordsBusy
                onClicked: dialogs.openCreateRoot()
            }
        }

        TextField {
            id: searchInput
            Layout.fillWidth: true
            placeholderText: qsTr("Search keywords")
            selectByMouse: true
            onTextChanged: root.searchText = text
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.allowFiltering
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Required keywords combine with AND; exclusions match any descendant.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            ShadowButton {
                compact: true
                variant: ShadowButton.Ghost
                text: qsTr("Clear keyword filters")
                enabled: root.controller.filterKeywordIdsAll.length > 0
                    || root.controller.filterExcludedKeywordIdsAny.length > 0
                onClicked: {
                    root.controller.filterKeywordIdsAll = []
                    root.controller.filterExcludedKeywordIdsAny = []
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 220
            radius: Theme.controlRadius
            color: Theme.surfaceSubtle
            border.width: 1
            border.color: Theme.border

            ListView {
                id: keywordList
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                spacing: 2
                model: root.visibleKeywords

                delegate: Rectangle {
                    id: keywordRow

                    required property var modelData
                    width: keywordList.width
                    height: 42
                    radius: Theme.compactControlRadius
                    color: rowHover.hovered
                        ? Theme.buttonGhostHover : Theme.transparent

                    readonly property string keywordId:
                        String(modelData.id)
                    readonly property bool assigned:
                        root.photoHasKeyword(keywordId)
                    readonly property bool requiredFilter:
                        root.containsId(
                            root.controller.filterKeywordIdsAll, keywordId)
                    readonly property bool excludedFilter:
                        root.containsId(
                            root.controller.filterExcludedKeywordIdsAny,
                            keywordId)

                    HoverHandler { id: rowHover }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8 + Number(
                            keywordRow.modelData.depth) * 16
                        anchors.rightMargin: 6
                        spacing: 6

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Label {
                                Layout.fillWidth: true
                                text: String(keywordRow.modelData.name)
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontBody
                                font.weight: keywordRow.assigned
                                    ? Font.DemiBold : Font.Normal
                                elide: Text.ElideRight
                            }

                            Label {
                                text: qsTr("%L1 photos").arg(
                                    Number(keywordRow.modelData.photoCount))
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontMeta
                            }
                        }

                        ShadowButton {
                            visible: root.allowFiltering
                            compact: true
                            minimumButtonWidth: 46
                            text: qsTr("Require")
                            selected: keywordRow.requiredFilter
                            toolTipText: qsTr(
                                "Require this keyword or a descendant")
                            onClicked: root.toggleRequired(
                                keywordRow.keywordId)
                        }

                        ShadowButton {
                            visible: root.allowFiltering
                            compact: true
                            minimumButtonWidth: 46
                            text: qsTr("Exclude")
                            selected: keywordRow.excludedFilter
                            toolTipText: qsTr(
                                "Exclude this keyword and its descendants")
                            onClicked: root.toggleExcluded(
                                keywordRow.keywordId)
                        }

                        ShadowButton {
                            visible: root.allowAssignment
                            compact: true
                            minimumButtonWidth: 42
                            text: qsTr("Add")
                            selected: keywordRow.assigned
                                && root.targets.length === 1
                            enabled: root.targets.length > 0
                                && !root.controller.libraryKeywordsBusy
                            toolTipText: qsTr(
                                "Add this keyword to selected photos")
                            onClicked:
                                root.controller.assignLibraryKeyword(
                                    keywordRow.keywordId, root.targets)
                        }

                        ShadowButton {
                            visible: root.allowAssignment
                            compact: true
                            minimumButtonWidth: 42
                            variant: ShadowButton.Ghost
                            text: qsTr("Remove")
                            enabled: root.targets.length > 0
                                && !root.controller.libraryKeywordsBusy
                            toolTipText: qsTr(
                                "Remove this keyword from selected photos")
                            onClicked:
                                root.controller.removeLibraryKeyword(
                                    keywordRow.keywordId, root.targets)
                        }

                        ShadowIconButton {
                            id: manageButton
                            visible: root.manageTaxonomy
                            source: "qrc:/icons/settings.svg"
                            buttonSize: 26
                            iconSize: 13
                            toolTipText: qsTr("Manage keyword")
                            accessibleName: toolTipText
                            onClicked: manageMenu.open()

                            Menu {
                                id: manageMenu

                                MenuItem {
                                    text: qsTr("Create child")
                                    onTriggered: dialogs.openCreateChild(
                                        keywordRow.keywordId,
                                        String(keywordRow.modelData.name))
                                }
                                MenuItem {
                                    text: qsTr("Rename")
                                    onTriggered: dialogs.openRename(
                                        keywordRow.keywordId,
                                        String(keywordRow.modelData.name))
                                }
                                MenuItem {
                                    text: qsTr("Move")
                                    onTriggered: dialogs.openMove(
                                        keywordRow.keywordId,
                                        String(keywordRow.modelData.name),
                                        String(keywordRow.modelData.parentId),
                                        Number(keywordRow.modelData.depth))
                                }
                                MenuSeparator {}
                                MenuItem {
                                    text: qsTr("Delete subtree")
                                    onTriggered: dialogs.openDelete(
                                        keywordRow.keywordId,
                                        String(keywordRow.modelData.name),
                                        Number(
                                            keywordRow.modelData.photoCount))
                                }
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: keywordList.count === 0
                        && !root.controller.libraryKeywordsBusy
                    text: root.searchText.length > 0
                        ? qsTr("No matching keywords")
                        : qsTr("Create a keyword to organize photos.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontBody
                }
            }

            BusyIndicator {
                anchors.centerIn: parent
                running: root.controller.libraryKeywordsBusy
                visible: running
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.allowAssignment && root.targets.length > 0
            text: qsTr("%L1 selected photos").arg(root.targets.length)
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
        }
    }
}
