pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: people

    required property var controller
    required property var aiPreferences
    readonly property int renderedGroupCount: peopleGroupRepeater.count
    readonly property int selectedGroupCount: controller.selectedGroupCount
    property string renameGroupId: ""
    property string renameCurrentName: ""

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
        const renderedGroups = controller.groups
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
        const renderedGroups = controller.groups
        if (!controller.busy && index >= 0 && index < renderedGroups.length)
            controller.toggleGroupSelection(String(renderedGroups[index].groupId))
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

            TextField {
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

    ScrollView {
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: Math.min(980, parent.width - 64)
            x: Math.round((parent.width - width) / 2)
            spacing: 18

            Item { Layout.preferredHeight: 30 }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 44
                    Layout.preferredHeight: 44
                    radius: 10
                    color: Theme.accentSurface

                    ShadowIcon {
                        anchors.centerIn: parent
                        source: "qrc:/icons/people.svg"
                        color: Theme.accent
                        size: 22
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3

                    Label {
                        text: qsTr("People")
                        color: Theme.textPrimary
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Find and organize recurring people with local face analysis.")
                        color: Theme.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: privacyContent.implicitHeight + 28
                radius: 10
                color: Theme.panel
                border.color: Theme.border

                RowLayout {
                    id: privacyContent
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 12

                    ShadowIcon {
                        source: "qrc:/icons/storage.svg"
                        color: Theme.textMuted
                        size: 18
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: qsTr("Local People data")
                            color: Theme.textPrimary
                            font.weight: Font.DemiBold
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Processing stays on this device. Face embeddings are not saved; groups, representative thumbnails, and your merges remain until you clear them.")
                            color: Theme.textMuted
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                ShadowButton {
                    objectName: "peopleStartButton"
                    text: !people.aiPreferences.peopleAnalysisExecutionAllowed
                        ? qsTr("Enable People")
                        : people.controller.hasResults
                            ? qsTr("Analyze Again") : qsTr("Start Analysis")
                    variant: ShadowButton.Primary
                    enabled: !people.controller.busy
                    onClicked: people.requestStartAnalysis()
                }

                ShadowButton {
                    objectName: "peopleCancelButton"
                    visible: people.controller.busy
                    text: people.controller.cancelRequested ? qsTr("Stopping…") : qsTr("Stop")
                    enabled: !people.controller.cancelRequested
                    onClicked: people.requestCancelAnalysis()
                }

                ShadowButton {
                    objectName: "peopleClearButton"
                    visible: people.controller.hasResults
                    text: qsTr("Clear People Data")
                    enabled: !people.controller.busy
                    onClicked: people.requestClearPeopleData()
                }

                BusyIndicator {
                    visible: people.controller.busy
                    running: visible
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                }

                Label {
                    Layout.fillWidth: true
                    text: people.controller.statusText
                    color: people.controller.errorText.length > 0
                        ? Theme.errorText : Theme.textMuted
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                visible: people.controller.errorText.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: errorLabel.implicitHeight + 24
                radius: 8
                color: Theme.dangerSurface

                Label {
                    id: errorLabel
                    anchors.fill: parent
                    anchors.margins: 12
                    text: people.controller.errorText
                    color: Theme.errorText
                    wrapMode: Text.WordWrap
                }
            }

            GridLayout {
                visible: people.controller.hasResults
                Layout.fillWidth: true
                columns: width >= 760 ? 4 : width >= 520 ? 2 : 1
                columnSpacing: 10
                rowSpacing: 10

                Repeater {
                    model: [
                        {
                            label: qsTr("Photos analyzed"),
                            value: people.controller.analyzedPhotos
                        },
                        {
                            label: qsTr("Faces found"),
                            value: people.controller.detectedFaces
                        },
                        {
                            label: qsTr("Faces compared"),
                            value: people.controller.embeddedFaces
                        },
                        {
                            label: qsTr("Anonymous groups"),
                            value: people.controller.groups.length
                        }
                    ]

                    delegate: Rectangle {
                        id: statisticCard

                        required property var modelData

                        Layout.fillWidth: true
                        Layout.preferredHeight: 72
                        radius: 9
                        color: Theme.panel
                        border.color: Theme.border

                        Column {
                            anchors.centerIn: parent
                            spacing: 3

                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: Number(statisticCard.modelData.value).toLocaleString()
                                color: Theme.textPrimary
                                font.pixelSize: 20
                                font.weight: Font.DemiBold
                            }

                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: String(statisticCard.modelData.label)
                                color: Theme.textMuted
                                font.pixelSize: 10
                            }
                        }
                    }
                }
            }

            Rectangle {
                visible: people.controller.groups.length > 1
                    || people.controller.canUndoMerge
                Layout.fillWidth: true
                implicitHeight: mergeContent.implicitHeight + 24
                radius: 10
                color: Theme.panel
                border.color: Theme.border

                RowLayout {
                    id: mergeContent
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: qsTr("Merge recognition results")
                            color: Theme.textPrimary
                            font.weight: Font.DemiBold
                        }

                        Label {
                            Layout.fillWidth: true
                            text: people.controller.mergeSelectionText
                            color: people.controller.selectedGroupCount >= 2
                                && !people.controller.canMergeSelectedGroups
                                ? Theme.warningText : Theme.textMuted
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }

                    ShadowButton {
                        objectName: "peopleUndoMergeButton"
                        visible: people.controller.canUndoMerge
                        text: qsTr("Undo merge")
                        enabled: !people.controller.busy
                        variant: ShadowButton.Ghost
                        onClicked: people.requestUndoMerge()
                    }

                    ShadowButton {
                        objectName: "peopleMergeButton"
                        text: qsTr("Merge selected")
                        enabled: !people.controller.busy
                            && people.controller.canMergeSelectedGroups
                        variant: ShadowButton.Primary
                        onClicked: people.requestMergeSelection()
                    }
                }
            }

            Label {
                visible: people.controller.truncated
                Layout.fillWidth: true
                text: qsTr("This preview reached its safety limit. A later background workflow can continue incrementally.")
                color: Theme.warningText
                wrapMode: Text.WordWrap
            }

            Label {
                visible: people.controller.hasResults
                    && people.controller.groups.length === 0
                Layout.fillWidth: true
                Layout.topMargin: 30
                text: qsTr("No recurring people were grouped in the current Library preview.")
                color: Theme.textMuted
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            GridLayout {
                visible: people.controller.groups.length > 0
                Layout.fillWidth: true
                columns: width >= 820 ? 4 : width >= 560 ? 3 : 2
                columnSpacing: 14
                rowSpacing: 14

                Repeater {
                    id: peopleGroupRepeater
                    model: people.controller.groups

                    delegate: Rectangle {
                        required property int index
                        required property var modelData
                        readonly property bool selected:
                            Boolean(modelData.selected)

                        objectName: "peopleGroupCard"

                        Layout.fillWidth: true
                        Layout.preferredHeight: 218
                        radius: 12
                        color: selected ? Theme.accentSurface : Theme.panelRaised
                        border.width: selected ? 2 : 1
                        border.color: selected ? Theme.accent : Theme.border
                        Accessible.role: Accessible.Button
                        Accessible.name: String(modelData.displayName || "").length > 0
                            ? qsTr("%1, %n photos", "", modelData.photoCount)
                                .arg(String(modelData.displayName))
                            : qsTr("Person %1, %n photos", "", modelData.photoCount)
                                .arg(modelData.displayIndex)
                        Accessible.checked: selected

                        TapHandler {
                            onTapped: people.requestToggleRenderedGroup(peopleGroup.index)
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 9

                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 76
                                Layout.preferredHeight: 76
                                radius: 10
                                color: Theme.accentSurface

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 3
                                    source: String(
                                        peopleGroup.modelData.thumbnailSource || "")
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    cache: false
                                    smooth: true
                                    mipmap: true
                                    visible: String(source).length > 0
                                }

                                ShadowIcon {
                                    anchors.centerIn: parent
                                    source: "qrc:/icons/people.svg"
                                    color: Theme.accent
                                    size: 34
                                    visible: String(
                                        peopleGroup.modelData.thumbnailSource || "").length === 0
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: String(peopleGroup.modelData.displayName || "").length > 0
                                    ? String(peopleGroup.modelData.displayName)
                                    : qsTr("Person %1").arg(
                                        peopleGroup.modelData.displayIndex)
                                color: Theme.textPrimary
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignHCenter
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%n photos", "", peopleGroup.modelData.photoCount)
                                color: Theme.textMuted
                                font.pixelSize: 11
                                horizontalAlignment: Text.AlignHCenter
                            }

                            ShadowButton {
                                objectName: "peopleNameButton"
                                Layout.alignment: Qt.AlignHCenter
                                text: String(peopleGroup.modelData.displayName || "").length > 0
                                    ? qsTr("Rename") : qsTr("Name")
                                enabled: !people.controller.busy
                                variant: ShadowButton.Ghost
                                onClicked: people.requestRenameRenderedGroup(
                                    peopleGroup.index)
                            }

                            Label {
                                visible: Boolean(peopleGroup.modelData.merged)
                                Layout.fillWidth: true
                                text: qsTr("Merged by you")
                                color: Theme.accent
                                font.pixelSize: 10
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }

                        id: peopleGroup
                    }
                }
            }

            Label {
                visible: people.controller.hasResults
                    && (people.controller.ungroupedFaces > 0
                        || people.controller.skippedItems > 0)
                Layout.fillWidth: true
                text: qsTr("%1 ungrouped faces · %2 skipped items")
                    .arg(people.controller.ungroupedFaces)
                    .arg(people.controller.skippedItems)
                color: Theme.textMuted
                font.pixelSize: 10
                horizontalAlignment: Text.AlignHCenter
            }

            Item { Layout.preferredHeight: 30 }
        }
    }
}
