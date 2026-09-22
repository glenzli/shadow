pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: people

    required property var controller
    required property var scope
    required property var aiPreferences
    readonly property int renderedGroupCount: peopleGrid.count
    readonly property int selectedGroupCount: controller.selectedGroupCount
    property bool mergeMode: false
    property string nameQuery: ""
    property bool useCurrentScope: true
    readonly property var visibleGroups: {
        const query = nameQuery.trim().toLocaleLowerCase()
        const scoped = useCurrentScope && scope.active
        return controller.groups.filter(group =>
            (!scoped || (scope.ready && Number(scope.counts[group.groupId] || 0) > 0))
            && (query.length === 0
                || String(group.displayName || "").toLocaleLowerCase().includes(query)))
    }
    property string renameGroupId: ""
    property string renameCurrentName: ""

    onVisibleChanged: {
        if (visible)
            useCurrentScope = true
        scope.setViewActive(visible)
    }
    Component.onCompleted: scope.setViewActive(visible)

    function requestStartAnalysis() {
        if (controller.busy)
            return
        if (!aiPreferences.peopleAnalysisExecutionAllowed) {
            peopleConsentDialog.open()
            return
        }
        controller.startAnalysis()
    }

    function requestClearPeopleData() {
        if (!controller.busy && controller.hasResults)
            clearPeopleDataDialog.open()
    }

    function requestCancelAnalysis() {
        if (controller.busy && !controller.cancelRequested)
            controller.cancelAnalysis()
    }

    function requestMergeSelection() {
        if (!controller.busy && controller.canMergeSelectedGroups)
            controller.mergeSelectedGroups()
    }

    function requestUndoMerge() {
        if (!controller.busy && controller.canUndoMerge)
            controller.undoLastMerge()
    }

    function requestRenameRenderedGroup(index) {
        const renderedGroups = visibleGroups
        if (controller.busy || index < 0 || index >= renderedGroups.length)
            return
        renameGroupId = String(renderedGroups[index].groupId)
        renameCurrentName = String(renderedGroups[index].displayName || "")
        personNameField.text = renameCurrentName
        personNameDialog.open()
        personNameField.forceActiveFocus()
        personNameField.selectAll()
    }

    function requestToggleRenderedGroup(index) {
        const renderedGroups = visibleGroups
        if (index < 0 || index >= renderedGroups.length) return
        if (mergeMode) {
            if (!controller.busy) controller.toggleGroupSelection(String(renderedGroups[index].groupId))
        } else controller.openGroup(String(renderedGroups[index].groupId))
    }

    Connections {
        target: people.controller
        ignoreUnknownSignals: true

        function onAuthorizationRequired() {
            peopleConsentDialog.open()
        }
    }

    Dialog {
        id: peopleConsentDialog
        objectName: "peopleConsentDialog"
        anchors.centerIn: parent
        width: Math.min(520, parent.width - 48)
        modal: true
        title: qsTr("Enable People?")
        standardButtons: Dialog.Cancel | Dialog.Ok

        onAccepted: {
            people.aiPreferences.grantPeopleAnalysisConsent()
            people.controller.startAnalysis()
        }
        onRejected: {
            if (!people.aiPreferences.peopleAnalysisConsentDecided)
                people.aiPreferences.denyPeopleAnalysisConsent()
        }

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow will analyze faces in this Library and organize recurring people across photos. Photos, face features, and people data stay on this device.")
                color: Theme.textPrimary
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("You can stop new analysis or clear all people data at any time in Settings.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }
        }
    }

    Dialog {
        id: clearPeopleDataDialog
        objectName: "clearPeopleDataDialog"
        anchors.centerIn: parent
        width: Math.min(480, parent.width - 48)
        modal: true
        title: qsTr("Clear people data?")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: people.controller.clearPeopleData()

        Label {
            width: 420
            text: qsTr("Stored face references, people groups, thumbnails, and your merges will be removed. Original photos and edits are not changed.")
            color: Theme.textPrimary
            wrapMode: Text.WordWrap
        }
    }

    Dialog {
        id: personNameDialog
        objectName: "peopleNameDialog"
        anchors.centerIn: parent
        width: Math.min(420, parent.width - 48)
        modal: true
        title: people.renameCurrentName.length > 0
            ? qsTr("Rename person") : qsTr("Name person")
        standardButtons: Dialog.Cancel | Dialog.Save
        onAccepted: {
            people.controller.renameGroup(people.renameGroupId,
                                          personNameField.text)
            people.renameGroupId = ""
            people.renameCurrentName = ""
        }
        onRejected: {
            people.renameGroupId = ""
            people.renameCurrentName = ""
        }

        contentItem: ColumnLayout {
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Names stay in this device's local People data.")
                color: Theme.textMuted
                wrapMode: Text.WordWrap
            }

            ShadowTextField {
                id: personNameField
                objectName: "peopleNameField"
                Layout.fillWidth: true
                maximumLength: 80
                selectByMouse: true
                placeholderText: qsTr("Person name")
                Accessible.name: qsTr("Person name")
                onAccepted: personNameDialog.accept()
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.window
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: qsTr("People")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontHeading
                font.weight: Font.DemiBold
            }
            ShadowButton {
                objectName: "peopleStartButton"
                text: !people.aiPreferences.peopleAnalysisExecutionAllowed
                    ? qsTr("Enable People") : people.controller.hasResults
                        ? (people.controller.truncated ? qsTr("Continue analysis") : qsTr("Check for new photos"))
                        : qsTr("Start Analysis")
                variant: ShadowButton.Primary
                enabled: !people.controller.busy
                onClicked: people.requestStartAnalysis()
            }
            ShadowButton {
                objectName: "peopleCancelButton"
                visible: people.controller.busy
                text: people.controller.cancelRequested ? qsTr("Stopping…") : qsTr("Stop")
                enabled: !people.controller.cancelRequested
                variant: ShadowButton.Ghost
                onClicked: people.requestCancelAnalysis()
            }
            ShadowIconButton {
                visible: people.controller.hasResults
                source: "qrc:/icons/refresh.svg"
                toolTipText: qsTr("Reanalyze all photos, keeping names and corrections")
                enabled: !people.controller.busy && people.aiPreferences.peopleAnalysisExecutionAllowed
                onClicked: people.controller.reanalyzeAll()
            }
            ShadowIconButton {
                objectName: "peopleClearButton"
                visible: people.controller.hasResults
                source: "qrc:/icons/trash.svg"
                toolTipText: qsTr("Clear People Data")
                enabled: !people.controller.busy
                onClicked: people.requestClearPeopleData()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            BusyIndicator { visible: people.controller.busy; running: visible; implicitWidth: 20; implicitHeight: 20 }
            Label {
                Layout.fillWidth: true
                text: people.controller.errorText.length > 0 ? people.controller.errorText : people.controller.statusText
                color: people.controller.errorText.length > 0 ? Theme.errorText : Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.Wrap
            }
            Label {
                visible: people.controller.hasResults
                text: qsTr("%1 photos checked · %2 people").arg(people.controller.analyzedPhotos).arg(people.controller.groups.length)
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: people.scope.active
            spacing: 8
            ShadowButton {
                objectName: "peopleCurrentScopeButton"
                text: qsTr("Current album and filters")
                variant: people.useCurrentScope ? ShadowButton.Primary : ShadowButton.Ghost
                onClicked: people.useCurrentScope = true
            }
            ShadowButton {
                objectName: "peopleAllLibraryButton"
                text: qsTr("All Library")
                variant: people.useCurrentScope ? ShadowButton.Ghost : ShadowButton.Primary
                onClicked: people.useCurrentScope = false
            }
            BusyIndicator {
                visible: people.useCurrentScope && people.scope.busy
                running: visible
                implicitWidth: 20
                implicitHeight: 20
            }
            Label {
                Layout.fillWidth: true
                visible: people.useCurrentScope
                text: people.scope.errorText.length > 0 ? people.scope.errorText
                    : people.scope.busy ? qsTr("Counting people in the current scope…")
                    : qsTr("People shown here match the current album and filters.")
                color: people.scope.errorText.length > 0 ? Theme.errorText : Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.Wrap
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: people.controller.groups.length > 0
            spacing: 12
            ShadowTextField {
                id: nameSearch
                objectName: "peopleNameSearch"
                Layout.fillWidth: true
                placeholderText: qsTr("Find a person by name…")
                onTextChanged: people.nameQuery = text
            }
            ShadowButton {
                objectName: "peopleUndoMergeButton"
                text: qsTr("Undo correction")
                visible: people.controller.canUndoMerge
                enabled: !people.controller.busy
                variant: ShadowButton.Ghost
                onClicked: people.requestUndoMerge()
            }
            ShadowButton {
                objectName: "peopleSelectionModeButton"
                text: people.mergeMode ? qsTr("Done") : qsTr("Select")
                enabled: !people.controller.busy
                variant: ShadowButton.Ghost
                onClicked: {
                    people.mergeMode = !people.mergeMode
                    if (!people.mergeMode) {
                        const groups = people.controller.groups
                        for (let i = 0; i < groups.length; ++i)
                            if (groups[i].selected) people.controller.toggleGroupSelection(groups[i].groupId)
                    }
                }
            }
            ShadowButton {
                objectName: "peopleMergeButton"
                visible: people.mergeMode
                text: qsTr("Merge selected")
                enabled: !people.controller.busy && people.controller.canMergeSelectedGroups
                variant: ShadowButton.Primary
                onClicked: people.requestMergeSelection()
            }
        }
        Label {
            visible: people.mergeMode
            Layout.fillWidth: true
            text: people.controller.mergeSelectionText
            color: Theme.textSecondary
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontMeta
        }
        Label {
            visible: people.controller.truncated && !people.controller.busy
            Layout.fillWidth: true
            text: qsTr("This batch is saved. Continue to analyze the remaining photos; completed photos will be reused.")
            color: Theme.warningText
            wrapMode: Text.Wrap
        }
        GridView {
            id: peopleGrid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: people.visibleGroups
            cellWidth: width / Math.max(1, Math.floor(width / 180))
            cellHeight: 204
            cacheBuffer: cellHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
            delegate: Item {
                id: personCard
                required property int index
                required property var modelData
                width: peopleGrid.cellWidth
                height: peopleGrid.cellHeight
                readonly property bool selected: people.mergeMode && Boolean(modelData.selected)
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 6
                    radius: Theme.controlRadius
                    color: personCard.selected ? Theme.accentSurfaceQuiet : Theme.panelRaised
                    border.color: personCard.selected ? Theme.accent : Theme.border
                    border.width: personCard.selected ? 2 : 1
                    objectName: "peopleGroupCard"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: String(personCard.modelData.displayName || "").length > 0
                        ? String(personCard.modelData.displayName) : qsTr("Person %1").arg(personCard.modelData.displayIndex)
                    Accessible.checked: personCard.selected
                    Accessible.onPressAction: people.requestToggleRenderedGroup(personCard.index)
                    Keys.onReturnPressed: people.requestToggleRenderedGroup(personCard.index)
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: people.requestToggleRenderedGroup(personCard.index)
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6
                        Item {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 72
                            Layout.preferredHeight: 72
                            Image {
                                anchors.fill: parent
                                source: String(personCard.modelData.thumbnailSource || "")
                                asynchronous: true
                                fillMode: Image.PreserveAspectCrop
                                mipmap: true
                            }
                            ShadowIcon {
                                anchors.centerIn: parent
                                visible: String(personCard.modelData.thumbnailSource || "").length === 0
                                source: "qrc:/icons/people.svg"
                                color: Theme.textMuted
                                size: 36
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: String(personCard.modelData.displayName || "").length > 0
                                ? String(personCard.modelData.displayName) : qsTr("Person %1").arg(personCard.modelData.displayIndex)
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSection
                            font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("%n photos", "", people.useCurrentScope && people.scope.active
                                ? Number(people.scope.counts[personCard.modelData.groupId] || 0)
                                : personCard.modelData.photoCount)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                            horizontalAlignment: Text.AlignHCenter
                        }
                        ShadowButton {
                            objectName: "peopleNameButton"
                            Layout.alignment: Qt.AlignHCenter
                            text: String(personCard.modelData.displayName || "").length > 0 ? qsTr("Rename") : qsTr("Name")
                            enabled: !people.controller.busy
                            variant: ShadowButton.Ghost
                            onClicked: people.requestRenameRenderedGroup(personCard.index)
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                width: Math.min(parent.width - 32, 460)
                visible: peopleGrid.count === 0
                text: people.useCurrentScope && people.scope.active && people.scope.busy
                    ? qsTr("Counting people in the current scope…")
                    : people.useCurrentScope && people.scope.active && people.scope.errorText.length > 0
                    ? people.scope.errorText
                    : people.nameQuery.length > 0 ? qsTr("No matching people")
                    : people.useCurrentScope && people.scope.active && people.controller.hasResults
                    ? qsTr("No analyzed people in the current scope.")
                    : people.controller.hasResults ? qsTr("No faces were found in the analyzed photos.")
                    : qsTr("Find and organize people locally. Open a person to browse their photos.")
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
        }
    }
}
