pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    required property var locationSearch
    property var libraryCandidates: []
    signal locationSelected(string key, string label)

    spacing: 7

    function chooseLocation(candidate) {
        if (!candidate)
            return
        const key = String(candidate.key)
        const label = String(candidate.label)
        if (key.length === 0 || label.length === 0)
            return
        locationSelected(key, label)
        resetQuery()
    }

    function chooseSearchResult(index) {
        if (index < 0 || index >= locationSearch.results.length)
            return
        chooseLocation(locationSearch.results[index])
    }

    function resetQuery() {
        searchDelay.stop()
        searchField.text = ""
        locationSearch.clear()
    }

    function synchronizeLibraryIndex() { libraryCombo.currentIndex = -1 }

    onLibraryCandidatesChanged: synchronizeLibraryIndex()

    Timer {
        id: searchDelay
        interval: 180
        repeat: false
        onTriggered: root.locationSearch.search(searchField.text)
    }

    TextField {
        id: searchField
        objectName: "personalProfileLocationSearchField"
        Layout.fillWidth: true
        placeholderText: qsTr("Search cities or regions")
        font.pixelSize: Theme.fontBody
        selectByMouse: true
        inputMethodHints: Qt.ImhNoPredictiveText
        Accessible.name: qsTr("Search living place")
        onTextEdited: searchDelay.restart()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? searchResults.implicitHeight + 8 : 0
        visible: root.locationSearch.results.length > 0
        color: Theme.panel
        border.width: 1
        border.color: Theme.border
        radius: Theme.controlRadius

        Column {
            id: searchResults
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 4

            Repeater {
                model: root.locationSearch.results

                delegate: Rectangle {
                    id: resultRow
                    required property var modelData
                    width: searchResults.width
                    height: 36
                    radius: Theme.compactControlRadius
                    color: resultTap.hovered ? Theme.buttonGhostHover : Theme.transparent

                    Label {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: String(resultRow.modelData.label)
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }

                    HoverHandler { id: resultTap }
                    TapHandler {
                        onTapped: root.chooseLocation(resultRow.modelData)
                    }
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        visible: root.locationSearch.busy
            || String(root.locationSearch.errorText).length > 0
            || (searchField.text.trim().length >= 2
                && String(root.locationSearch.activeQuery) === searchField.text.trim()
                && root.locationSearch.results.length === 0)
        text: root.locationSearch.busy
            ? qsTr("Searching offline city data…")
            : String(root.locationSearch.errorText).length > 0
                ? root.locationSearch.errorText : qsTr("No matching city or region")
        color: String(root.locationSearch.errorText).length > 0
            ? Theme.errorText : Theme.textMuted
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.Wrap
    }

    Label {
        Layout.fillWidth: true
        visible: root.libraryCandidates.length > 0
        text: qsTr("Or choose a place already found in your Library")
        color: Theme.textMuted
        font.pixelSize: Theme.fontMeta
    }

    ShadowComboBox {
        id: libraryCombo
        objectName: "personalProfileLocationLibraryCombo"
        Layout.fillWidth: true
        visible: root.libraryCandidates.length > 0
        model: root.libraryCandidates
        textRole: "label"
        valueRole: "key"
        displayText: currentIndex >= 0 ? currentText : qsTr("Choose from Library places")
        font.pixelSize: Theme.fontBody
        onActivated: index => root.chooseLocation(root.libraryCandidates[index])
    }
}
