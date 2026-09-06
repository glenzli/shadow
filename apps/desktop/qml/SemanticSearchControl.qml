pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns semantic-search input, waiting/error projection, and the responsive
// transition from a toolbar field to an anchored compact popup.
Item {
    id: control

    required property var workspace
    property bool expanded: false
    property string draftQuery: ""
    readonly property var semantic: workspace.semanticSearchController

    implicitWidth: expanded ? 268 : Theme.compactControlHeight
    implicitHeight: Theme.compactControlHeight

    function submit(query) {
        const normalizedQuery = String(query || "").trim()
        if (normalizedQuery.length === 0)
            return
        draftQuery = normalizedQuery
        workspace.galleryPresentation = ReviewWorkspace.JustifiedGrid
        semantic.search(normalizedQuery)
    }

    function clearSearch() {
        draftQuery = ""
        semantic.clearSessionResults()
        compactPopup.close()
    }

    function resultSummary() {
        if (!semantic.hasResults)
            return ""
        const summary = qsTr("%1 ranked photos · %2 checked · %3 skipped").arg(
            semantic.shownResultCount).arg(semantic.consideredPhotos).arg(semantic.skippedItems)
        return summary + (semantic.truncated
            ? qsTr(" · Library coverage is incomplete") : qsTr(" · Current Library scan complete"))
            + qsTr(". Similarity order is not a confidence score.")
    }

    component SearchField: Rectangle {
        id: field

        required property var owner
        required property string inputObjectName
        property bool popupPresentation: false

        function forceInputFocus() {
            semanticInput.forceActiveFocus()
            semanticInput.selectAll()
        }

        implicitHeight: 30
        radius: 8
        color: owner.semantic.busy || owner.semantic.hasResults
            ? Theme.accentSurfaceQuiet : Theme.surfaceSubtle
        border.width: 1
        border.color: owner.semantic.errorText.length > 0
            ? Theme.dangerBorder
            : owner.semantic.busy || semanticInput.activeFocus
                ? Theme.accent : Theme.border

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 5
            spacing: 5

            ShadowIcon {
                source: "qrc:/icons/zoom.svg"
                color: field.owner.semantic.busy
                    || field.owner.semantic.hasResults
                    ? Theme.accent : Theme.textMuted
                size: 14
            }

            TextField {
                id: semanticInput
                objectName: field.inputObjectName
                Layout.fillWidth: true
                Layout.fillHeight: true
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                text: field.owner.draftQuery
                placeholderText: qsTr("Describe a photo…")
                color: Theme.textPrimary
                placeholderTextColor: Theme.textPlaceholder
                font.pixelSize: 11
                visible: true
                selectByMouse: true
                background: Item {}
                onTextEdited: field.owner.draftQuery = text
                onAccepted: field.owner.submit(text)
            }

            Label {
                objectName: "semanticSearchWaitingText"
                visible: false
                Layout.fillWidth: true
                text: qsTr("Matching locally…")
                color: Theme.accent
                font.pixelSize: 11
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            BusyIndicator {
                visible: field.owner.semantic.busy
                running: visible
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
            }

            ShadowIconButton {
                objectName: "semanticSearchSubmitButton"
                visible: field.owner.draftQuery.trim().length > 0
                    && (!field.owner.semantic.hasResults
                        || field.owner.draftQuery.trim()
                            !== field.owner.semantic.activeQuery)
                buttonSize: 20
                iconSize: 12
                source: "qrc:/icons/zoom.svg"
                toolTipText: qsTr("Search by description")
                accessibleName: toolTipText
                onClicked: field.owner.submit(field.owner.draftQuery)
            }

            ShadowIconButton {
                objectName: "semanticSearchClearButton"
                visible: field.owner.semantic.busy
                    || (field.owner.draftQuery.length > 0
                        || field.owner.semantic.hasResults)
                buttonSize: 20
                iconSize: 11
                source: "qrc:/icons/clear.svg"
                toolTipText: field.owner.semantic.busy ? qsTr("Cancel semantic search") : qsTr("Clear semantic filter")
                accessibleName: toolTipText
                onClicked: field.owner.clearSearch()
            }
        }

        HoverHandler { id: fieldHover }

        ToolTip.visible: !field.popupPresentation && fieldHover.hovered
            && (field.owner.semantic.errorText.length > 0
                || field.owner.resultSummary().length > 0)
        ToolTip.text: field.owner.semantic.errorText.length > 0
            ? field.owner.semantic.errorText : field.owner.resultSummary()
    }

    SearchField {
        id: inlineField
        objectName: "semanticSearchField"
        anchors.fill: parent
        visible: control.expanded
        owner: control
        inputObjectName: "semanticSearchInput"
    }

    ShadowIconButton {
        id: compactButton
        objectName: "semanticSearchCompactButton"
        anchors.centerIn: parent
        visible: !control.expanded
        enabled: true
        selected: compactPopup.visible || control.semantic.hasResults
        variant: selected ? ShadowIconButton.Tinted : ShadowIconButton.Ghost
        source: "qrc:/icons/zoom.svg"
        toolTipText: control.semantic.hasResults
            ? control.resultSummary() : qsTr("Search photos by description")
        accessibleName: qsTr("Search photos by description")
        onClicked: compactPopup.open()
    }

    BusyIndicator {
        anchors.centerIn: compactButton
        visible: !control.expanded && control.semantic.busy
        running: visible
        width: 20
        height: 20
    }

    Popup {
        id: compactPopup
        objectName: "semanticSearchPopup"
        // Let Popup perform the trigger-to-overlay mapping. Using already
        // transformed full-window coordinates here double-counted the native
        // frame offset on macOS and shifted the field toward the sidebar.
        parent: control
        width: 360
        height: popupColumn.implicitHeight + topPadding + bottomPadding
        topPadding: 12
        bottomPadding: 12
        leftPadding: 12
        rightPadding: 12
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        x: control.width - width
        y: control.height + 6
        onOpened: popupField.forceInputFocus()

        background: Rectangle {
            radius: 10
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            id: popupColumn
            spacing: 8

            SearchField {
                id: popupField
                Layout.fillWidth: true
                owner: control
                inputObjectName: "semanticSearchPopupInput"
                popupPresentation: true
            }

            Label {
                visible: control.semantic.errorText.length > 0
                Layout.fillWidth: true
                text: control.semantic.errorText
                color: Theme.dangerText
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }

            Label {
                visible: control.semantic.errorText.length === 0
                    && control.semantic.hasResults
                Layout.fillWidth: true
                text: control.resultSummary()
                color: Theme.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
            }
        }
    }

    Connections {
        target: control.semantic

        function onResultsChanged() {
            if (!control.semantic.busy && !control.semantic.hasResults
                    && control.semantic.activeQuery.length === 0)
                control.draftQuery = ""
        }
    }
}
