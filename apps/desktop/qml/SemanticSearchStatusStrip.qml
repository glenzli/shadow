pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the semantic-search field and visible lifecycle projection below the
// Review toolbar. Keeping this filter row separate prevents gallery controls
// from collapsing search at narrower window sizes.
Rectangle {
    id: strip

    required property var workspace
    readonly property var semantic: workspace.semanticSearchController
    visible: !workspace.comparison.compareMode
        && !workspace.culling.arenaActive
    height: visible ? 38 : 0
    color: semantic.errorText.length > 0
        ? Theme.dangerSurface
        : semantic.busy || semantic.hasResults
            ? Theme.accentSurfaceQuiet : Theme.chrome

    function submitSemanticSearch(query) {
        const normalizedQuery = query.trim()
        if (normalizedQuery.length === 0)
            return
        strip.workspace.galleryPresentation = ReviewWorkspace.JustifiedGrid
        strip.semantic.search(normalizedQuery)
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: strip.semantic.errorText.length > 0
            ? Theme.dangerBorder : Theme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 8

        Rectangle {
            id: semanticSearchField
            objectName: "semanticSearchField"
            Layout.preferredWidth: 250
            Layout.minimumWidth: 190
            Layout.preferredHeight: 30
            radius: 8
            color: strip.semantic.busy || strip.semantic.hasResults
                ? Theme.accentSurfaceQuiet : Theme.surfaceSubtle
            border.color: strip.semantic.busy || semanticInput.activeFocus
                ? Theme.accent : Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 5
                spacing: 5

                ShadowIcon {
                    source: "qrc:/icons/zoom.svg"
                    color: strip.semantic.busy || strip.semantic.hasResults
                        ? Theme.accent : Theme.textMuted
                    size: 14
                }

                TextField {
                    id: semanticInput
                    objectName: "semanticSearchInput"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 0
                    bottomPadding: 0
                    placeholderText: qsTr("Describe a photo…")
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.textPlaceholder
                    font.pixelSize: 11
                    visible: !strip.semantic.busy
                    selectByMouse: true
                    background: Item {}
                    onAccepted: strip.submitSemanticSearch(text)
                }

                Label {
                    objectName: "semanticSearchWaitingText"
                    visible: strip.semantic.busy
                    Layout.fillWidth: true
                    text: qsTr("Matching locally…")
                    color: Theme.accent
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                BusyIndicator {
                    visible: strip.semantic.busy
                    running: visible
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                }

                ShadowIconButton {
                    objectName: "semanticSearchSubmitButton"
                    visible: !strip.semantic.busy
                        && semanticInput.text.trim().length > 0
                        && (!strip.semantic.hasResults
                            || semanticInput.text.trim()
                                !== strip.semantic.activeQuery)
                    buttonSize: 20
                    iconSize: 12
                    source: "qrc:/icons/zoom.svg"
                    toolTipText: qsTr("Search by description")
                    accessibleName: toolTipText
                    onClicked: strip.submitSemanticSearch(semanticInput.text)
                }

                ShadowIconButton {
                    objectName: "semanticSearchClearButton"
                    visible: !strip.semantic.busy
                        && (semanticInput.text.length > 0
                            || strip.semantic.hasResults)
                    buttonSize: 20
                    iconSize: 11
                    source: "qrc:/icons/clear.svg"
                    toolTipText: qsTr("Clear semantic filter")
                    accessibleName: toolTipText
                    onClicked: {
                        semanticInput.clear()
                        strip.semantic.clearSessionResults()
                    }
                }
            }

            ToolTip.visible: semanticSearchHover.hovered
                && strip.semantic.errorText.length > 0
            ToolTip.text: strip.semantic.errorText

            HoverHandler { id: semanticSearchHover }

            Connections {
                target: strip.semantic

                function onResultsChanged() {
                    if (!strip.semantic.busy && !strip.semantic.hasResults
                            && strip.semantic.activeQuery.length === 0)
                        semanticInput.clear()
                }
            }
        }

        Label {
            visible: strip.semantic.errorText.length > 0
            Layout.fillWidth: true
            text: strip.semantic.errorText
            color: Theme.dangerText
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        ShadowButton {
            objectName: "semanticHighRelevanceButton"
            visible: strip.semantic.hasResults
                && strip.semantic.highRelevanceCount > 0
            compact: true
            minimumButtonWidth: 0
            variant: ShadowButton.Ghost
            selected: strip.semantic.relevanceFilter === "all"
                || strip.semantic.relevanceFilter === "high"
            text: qsTr("Highly related %1").arg(
                strip.semantic.highRelevanceCount)
            toolTipText: qsTr("Show only the strongest semantic matches")
            onClicked: strip.semantic.setRelevanceFilter(
                strip.semantic.relevanceFilter === "high" ? "all" : "high")
        }

        ShadowButton {
            objectName: "semanticPossibleRelevanceButton"
            visible: strip.semantic.hasResults
                && strip.semantic.possibleRelevanceCount > 0
            compact: true
            minimumButtonWidth: 0
            variant: ShadowButton.Ghost
            selected: strip.semantic.relevanceFilter === "all"
                || strip.semantic.relevanceFilter === "possible"
            text: qsTr("Possibly related %1").arg(
                strip.semantic.possibleRelevanceCount)
            toolTipText: qsTr("Show the broader semantic matches")
            onClicked: strip.semantic.setRelevanceFilter(
                strip.semantic.relevanceFilter === "possible"
                    ? "all" : "possible")
        }

        Item { Layout.fillWidth: true }

        Label {
            visible: strip.semantic.hasResults
                && strip.semantic.hiddenLowRelevanceCount > 0
            text: qsTr("%1 weak hidden").arg(
                strip.semantic.hiddenLowRelevanceCount)
            color: Theme.textMuted
            font.pixelSize: 9
        }
    }
}
