pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    objectName: "libraryMapPlaceSearch"

    required property var service
    required property var mapController
    property bool providerEligible: true
    property bool proposeChosenCoordinate: false
    readonly property bool nativeSurfaceBlocked: visible
        && (service.busy || service.results.length > 0
            || String(service.errorText).length > 0)
    signal resultChosen(real latitude, real longitude, string name, string label)

    implicitWidth: Math.min(390, Math.max(240, parent ? parent.width - 28 : 390))
    implicitHeight: searchColumn.implicitHeight + 16
    width: implicitWidth
    height: implicitHeight
    radius: 8
    color: Theme.panelRaised
    border.width: 1
    border.color: Theme.borderStrong
    visible: providerEligible && (service.available || searchField.activeFocus)

    onProviderEligibleChanged: {
        if (!providerEligible) {
            searchDelay.stop();
            searchField.clear();
            service.clear();
        }
    }

    function submitSearch() {
        root.service.search(
            searchField.text,
            Number(root.mapController.centerLatitude),
            Number(root.mapController.centerLongitude));
    }

    function choose(result) {
        if (!result)
            return;
        mapController.navigateToContext(
            Number(result.latitude), Number(result.longitude),
            Math.max(Number(mapController.zoomLevel), 13));
        if (root.proposeChosenCoordinate) {
            root.mapController.setPlacementActive(true);
            root.mapController.setPendingCoordinate(
                true, Number(result.latitude), Number(result.longitude));
            root.resultChosen(
                Number(result.latitude), Number(result.longitude),
                String(result.name || ""), String(result.label || ""));
        }
        searchField.text = String(result.name);
        service.clear();
        searchField.focus = false;
    }

    ColumnLayout {
        id: searchColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 5

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            TextField {
                id: searchField
                objectName: "libraryMapAmapSearchField"
                Layout.fillWidth: true
                placeholderText: qsTr("Search places with AMap")
                selectByMouse: true
                Accessible.name: placeholderText
                onTextEdited: searchDelay.restart()
                onAccepted: {
                    searchDelay.stop();
                    root.submitSearch();
                }
            }

            BusyIndicator {
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                visible: root.service.busy
                running: visible
            }

            ShadowIconButton {
                visible: searchField.text.length > 0
                source: "qrc:/icons/close.svg"
                toolTipText: qsTr("Clear place search")
                accessibleName: toolTipText
                onClicked: {
                    searchDelay.stop();
                    searchField.clear();
                    root.service.clear();
                    searchField.forceActiveFocus();
                }
            }
        }

        Repeater {
            model: root.service.results

            delegate: Rectangle {
                id: resultRow
                objectName: "libraryMapAmapSearchResult"
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                radius: Theme.compactControlRadius
                color: resultHover.hovered ? Theme.buttonGhostHover : Theme.transparent

                Label {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 9
                    anchors.rightMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    text: String(resultRow.modelData.label)
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideRight
                }

                HoverHandler { id: resultHover }
                TapHandler { onTapped: root.choose(resultRow.modelData) }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !root.service.busy && String(root.service.errorText).length > 0
            text: String(root.service.errorText)
            color: Theme.errorText
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }
    }

    Timer {
        id: searchDelay
        interval: 240
        repeat: false
        onTriggered: root.submitSearch()
    }

    Connections {
        target: root.service

        function onStateChanged() {
            if (!root.service.available && !searchField.activeFocus) {
                searchDelay.stop();
                searchField.clear();
            }
        }
    }
}
