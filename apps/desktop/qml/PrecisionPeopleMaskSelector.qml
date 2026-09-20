pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Transient people and detail selection for one node-bound AI mask session.
// Person numbers and thumbnails are current-photo UI state, not identities.
ColumnLayout {
    id: selector

    required property var editor

    readonly property var people: editor.aiMaskPeople || []
    readonly property int selectedPerson: Number(editor.aiMaskSelectedPerson ?? -1)
    readonly property int selectedRegionMask: Number(editor.aiMaskFaceRegionMask || 0)
    readonly property int renderedPersonCount: peopleRepeater.count
    property var regions: [
        { label: qsTr("Face"), index: 0 },
        { label: qsTr("Skin"), index: 1 },
        { label: qsTr("Eyes"), index: 2 },
        { label: qsTr("Eyebrows"), index: 3 },
        { label: qsTr("Lips and mouth"), index: 4 },
        { label: qsTr("Nose"), index: 5 },
        { label: qsTr("Ears"), index: 6 },
        { label: qsTr("Hair"), index: 7 },
        { label: qsTr("Neck"), index: 8 },
        { label: qsTr("Clothing"), index: 9 },
        { label: qsTr("Accessories"), index: 10 }
    ]

    spacing: 8

    function selectPerson(personIndex) {
        if (!editor.aiMaskBusy)
            editor.aiMaskSelectedPerson = personIndex
    }

    function regionAvailable(regionIndex) {
        if (selectedPerson < 0 || selectedPerson >= people.length)
            return false
        const person = people[selectedPerson]
        return !Boolean(person.regionsAnalyzed)
            || (Number(person.availableRegionMask || 0) & (1 << regionIndex)) !== 0
    }

    function toggleRegion(regionIndex, selected) {
        if (!editor.aiMaskBusy && regionAvailable(regionIndex))
            editor.toggleAiMaskFaceRegion(regionIndex, selected)
    }

    Label {
        Layout.fillWidth: true
        text: selector.people.length > 0
            ? qsTr("People") : qsTr("Looking for people in this photo…")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontMeta
    }

    Flickable {
        id: peopleStrip

        objectName: "aiPeopleStrip"
        Layout.fillWidth: true
        Layout.preferredHeight: selector.people.length > 0 ? 86 : 0
        visible: selector.people.length > 0
        contentWidth: peopleRow.implicitWidth
        contentHeight: height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Row {
            id: peopleRow
            height: parent.height
            spacing: 8

            Repeater {
                id: peopleRepeater
                model: selector.people

                delegate: Rectangle {
                    id: personCard

                    required property int index
                    required property var modelData

                    readonly property int personIndex: Number(modelData.index ?? index)
                    readonly property bool selected: selector.selectedPerson === personIndex

                    objectName: "aiPersonCard" + index
                    width: 68
                    height: 82
                    radius: 8
                    color: selected ? Theme.accentSurface : Theme.surfaceSubtle
                    border.width: selected ? 2 : 1
                    border.color: selected ? Theme.accent : Theme.border
                    opacity: selector.editor.aiMaskBusy && !selected ? 0.58 : 1

                    Image {
                        anchors.top: parent.top
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.topMargin: 5
                        width: 56
                        height: 56
                        source: String(personCard.modelData.thumbnailSource || "")
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: false
                        smooth: true
                    }

                    Label {
                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottomMargin: 4
                        text: qsTr("Person %1").arg(personCard.personIndex + 1)
                        color: personCard.selected ? Theme.accent : Theme.textSecondary
                        font.pixelSize: Theme.fontCaption
                    }

                    TapHandler {
                        enabled: !selector.editor.aiMaskBusy
                        onTapped: selector.selectPerson(personCard.personIndex)
                    }
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        visible: selector.people.length > 0
        text: qsTr("Details")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontMeta
    }

    Flow {
        id: regionFlow

        objectName: "aiPeopleRegionList"
        Layout.fillWidth: true
        Layout.preferredHeight: childrenRect.height
        visible: selector.people.length > 0
        spacing: 6

        Repeater {
            model: selector.regions

            delegate: Button {
                id: regionButton

                required property int index
                required property var modelData

                readonly property int regionBit: 1 << Number(modelData.index)
                readonly property bool selected:
                    (selector.selectedRegionMask & regionBit) !== 0
                readonly property var person: selector.selectedPerson >= 0
                    && selector.selectedPerson < selector.people.length
                    ? selector.people[selector.selectedPerson] : null
                readonly property bool available: !person
                    || !Boolean(person.regionsAnalyzed)
                    || (Number(person.availableRegionMask || 0) & regionBit) !== 0

                objectName: "aiPeopleRegion" + index
                implicitHeight: 28
                leftPadding: 10
                rightPadding: 10
                text: String(modelData.label)
                enabled: !selector.editor.aiMaskBusy && selector.selectedPerson >= 0 && available
                opacity: enabled ? 1 : 0.38
                font.pixelSize: Theme.fontMeta
                onClicked: selector.toggleRegion(Number(modelData.index), !selected)

                contentItem: Label {
                    text: regionButton.text
                    color: regionButton.selected ? Theme.accent : Theme.textPrimary
                    font: regionButton.font
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 7
                    color: regionButton.selected ? Theme.accentSurface : Theme.surfaceSubtle
                    border.width: 1
                    border.color: regionButton.selected ? Theme.accent : Theme.border
                }
            }
        }
    }
}
