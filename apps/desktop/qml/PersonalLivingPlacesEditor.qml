pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    required property var locationSearch
    property var libraryCandidates: []
    property var places: []
    property int nextDraftId: 1
    property int editingIndex: -1
    property bool adding: false

    readonly property var activePlace: editingIndex >= 0 && editingIndex < places.length
        ? places[editingIndex] : ({})

    spacing: 10

    function currentMonth() {
        const now = new Date()
        return String(now.getFullYear()) + "-" + String(now.getMonth() + 1).padStart(2, "0")
    }

    function resetSearch() {
        locationField.resetQuery()
        adding = false
        editingIndex = -1
    }

    function synchronizeLibraryIndex() { locationField.synchronizeLibraryIndex() }

    function copyPlace(place) {
        return {
            "id": String(place.id),
            "key": String(place.key),
            "label": String(place.label),
            "startMonth": String(place.startMonth || ""),
            "endMonth": String(place.endMonth || "")
        }
    }

    function periodModeFor(place) {
        const startMonth = String(place.startMonth || "")
        const endMonth = String(place.endMonth || "")
        if (startMonth.length === 0 && endMonth.length === 0)
            return 0
        if (startMonth.length > 0 && endMonth.length === 0)
            return 1
        if (startMonth.length === 0)
            return 2
        return 3
    }

    function periodSummary(place) {
        const startMonth = String(place.startMonth || "")
        const endMonth = String(place.endMonth || "")
        if (startMonth.length === 0 && endMonth.length === 0)
            return ""
        if (startMonth.length > 0 && endMonth.length === 0)
            return startMonth + " →"
        if (startMonth.length === 0)
            return "→ " + endMonth
        return startMonth + " – " + endMonth
    }

    function beginAdding() {
        editingIndex = -1
        adding = true
        locationField.resetQuery()
    }

    function beginEditing(index) {
        if (index < 0 || index >= places.length)
            return
        adding = false
        editingIndex = index
    }

    function finishEditing() { editingIndex = -1 }

    function addPlace(key, label) {
        const normalizedKey = String(key)
        const normalizedLabel = String(label)
        if (normalizedKey.length === 0 || normalizedLabel.length === 0)
            return
        for (let index = 0; index < places.length; ++index) {
            const existing = places[index]
            if (String(existing.key) === normalizedKey
                    && String(existing.startMonth || "").length === 0
                    && String(existing.endMonth || "").length === 0) {
                beginEditing(index)
                return
            }
        }
        const updated = places.slice()
        updated.push({
            "id": "draft-" + Date.now() + "-" + nextDraftId++,
            "key": normalizedKey,
            "label": normalizedLabel,
            "startMonth": "",
            "endMonth": ""
        })
        places = updated
        adding = false
        editingIndex = updated.length - 1
    }

    function replacePlace(index, startMonth, endMonth) {
        if (index < 0 || index >= places.length)
            return
        const updated = places.slice()
        const place = copyPlace(updated[index])
        place.startMonth = String(startMonth)
        place.endMonth = String(endMonth)
        updated[index] = place
        places = updated
    }

    function removePlace(index) {
        if (index < 0 || index >= places.length)
            return
        const updated = places.slice()
        updated.splice(index, 1)
        places = updated
        editingIndex = -1
    }

    Flow {
        id: placeTags
        objectName: "personalProfileLivingPlaceTags"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(30, childrenRect.height)
        spacing: 7

        Repeater {
            model: root.places

            delegate: Button {
                id: placeTag
                required property var modelData
                required property int index

                objectName: "personalProfileLivingPlaceTag"
                width: Math.min(placeTags.width, chipContent.implicitWidth + 22)
                height: 30
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                Accessible.name: qsTr("Edit %1").arg(String(modelData.label))
                onClicked: root.beginEditing(index)

                background: Rectangle {
                    radius: placeTag.height / 2
                    color: placeTag.down
                        ? Theme.accentSurfacePressed
                        : root.editingIndex === placeTag.index || placeTag.hovered
                            ? Theme.accentSurface : Theme.accentSurfaceQuiet
                    border.width: 1
                    border.color: root.editingIndex === placeTag.index
                        ? Theme.accentBorder : Theme.border
                }

                contentItem: RowLayout {
                    id: chipContent
                    spacing: 6

                    ShadowIcon {
                        source: "qrc:/icons/location-pin.svg"
                        size: 13
                        color: Theme.accentSelectionText
                    }
                    Label {
                        text: String(placeTag.modelData.label)
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSection
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                        Layout.maximumWidth: 210
                    }
                    Label {
                        visible: text.length > 0
                        text: root.periodSummary(placeTag.modelData)
                        color: Theme.accentTextMuted
                        font.pixelSize: Theme.fontMeta
                    }
                    ShadowIcon {
                        source: "qrc:/icons/edit.svg"
                        size: 11
                        color: Theme.textMuted
                    }
                }
            }
        }

        ShadowButton {
            id: addButton
            objectName: "personalProfileLivingPlaceAddButton"
            compact: true
            variant: root.adding ? ShadowButton.Tinted : ShadowButton.Ghost
            text: qsTr("+ Add place")
            onClicked: root.adding ? root.resetSearch() : root.beginAdding()
        }
    }

    Rectangle {
        objectName: "personalProfileLivingPlaceAddPanel"
        Layout.fillWidth: true
        Layout.preferredHeight: addContent.implicitHeight + 20
        visible: root.adding
        color: Theme.panelInset
        border.width: 1
        border.color: Theme.border
        radius: Theme.controlRadius

        ColumnLayout {
            id: addContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 10
            spacing: 8

            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Add a living place")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSection
                    font.weight: Font.DemiBold
                }
                ShadowButton {
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Cancel")
                    onClicked: root.resetSearch()
                }
            }

            PersonalLocationSearchField {
                id: locationField
                objectName: "personalProfileLivingPlaceSearch"
                Layout.fillWidth: true
                locationSearch: root.locationSearch
                libraryCandidates: root.libraryCandidates
                onLocationSelected: (key, label) => root.addPlace(key, label)
            }
        }
    }

    Rectangle {
        id: editPanel
        objectName: "personalProfileLivingPlaceEditPanel"
        Layout.fillWidth: true
        Layout.preferredHeight: editContent.implicitHeight + 20
        visible: root.editingIndex >= 0
        color: Theme.panelInset
        border.width: 1
        border.color: Theme.accentBorder
        radius: Theme.controlRadius

        ColumnLayout {
            id: editContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 10
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    Label {
                        Layout.fillWidth: true
                        text: String(root.activePlace.label || "")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Label {
                        text: qsTr("When was this place part of ordinary life?")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                    }
                }
                ShadowButton {
                    id: finishButton
                    objectName: "personalProfileLivingPlaceEditorDoneButton"
                    compact: true
                    variant: ShadowButton.Tinted
                    text: qsTr("Done")
                    onClicked: root.finishEditing()
                }
            }

            ComboBox {
                id: periodMode
                objectName: "personalProfileLivingPlacePeriodMode"
                Layout.fillWidth: true
                model: [
                    qsTr("Always part of my life"),
                    qsTr("From a month onward"),
                    qsTr("Until a month"),
                    qsTr("During a month range")
                ]
                currentIndex: root.periodModeFor(root.activePlace)
                font.pixelSize: Theme.fontBody
                onActivated: index => {
                    const month = root.currentMonth()
                    const startMonth = String(root.activePlace.startMonth || "")
                    const endMonth = String(root.activePlace.endMonth || "")
                    if (index === 0)
                        root.replacePlace(root.editingIndex, "", "")
                    else if (index === 1)
                        root.replacePlace(root.editingIndex, startMonth || month, "")
                    else if (index === 2)
                        root.replacePlace(root.editingIndex, "", endMonth || month)
                    else
                        root.replacePlace(root.editingIndex,
                                          startMonth || month, endMonth || month)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: periodMode.currentIndex !== 0
                spacing: 8

                TextField {
                    objectName: "personalProfileLivingPlaceStartMonth"
                    Layout.fillWidth: true
                    visible: periodMode.currentIndex === 1 || periodMode.currentIndex === 3
                    text: String(root.activePlace.startMonth || "")
                    placeholderText: qsTr("Start · YYYY-MM")
                    font.pixelSize: Theme.fontBody
                    inputMethodHints: Qt.ImhDate
                    onEditingFinished: root.replacePlace(root.editingIndex, text,
                                                         String(root.activePlace.endMonth || ""))
                }
                TextField {
                    objectName: "personalProfileLivingPlaceEndMonth"
                    Layout.fillWidth: true
                    visible: periodMode.currentIndex === 2 || periodMode.currentIndex === 3
                    text: String(root.activePlace.endMonth || "")
                    placeholderText: qsTr("End · YYYY-MM")
                    font.pixelSize: Theme.fontBody
                    inputMethodHints: Qt.ImhDate
                    onEditingFinished: root.replacePlace(root.editingIndex,
                                                         String(root.activePlace.startMonth || ""),
                                                         text)
                }
            }

            RowLayout {
                Layout.fillWidth: true

                ShadowButton {
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Remove place")
                    onClicked: root.removePlace(root.editingIndex)
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: root.periodSummary(root.activePlace).length > 0
                    text: root.periodSummary(root.activePlace)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        visible: root.places.length === 0 && !root.adding
        text: qsTr("No living places yet. Add home, hometown, or a former home.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.Wrap
    }
}
