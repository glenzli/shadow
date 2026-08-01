pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    required property var controller
    readonly property bool hasFacetFilter:
        controller.filterCaptureMonth.length > 0
        || controller.filterCameraKey.length > 0
        || controller.filterLensKey.length > 0
        || controller.filterCountryKey.length > 0
        || controller.filterLocalityKey.length > 0

    parent: Overlay.overlay
    modal: false
    focus: true
    width: Math.min(380, parent.width - 32)
    implicitHeight: contentColumn.implicitHeight + topPadding + bottomPadding
    height: Math.min(implicitHeight, parent.height - 72)
    x: Math.round((parent.width - width) / 2)
    y: Math.min(58, Math.max(16, parent.height - height - 16))
    padding: 12
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: controller.refreshLibraryFacets()

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    component FacetGroup: ColumnLayout {
        id: facetGroup
        required property var controller
        required property string kind
        required property string title
        required property string selectedKey
        required property var values
        required property string emptyText

        Layout.fillWidth: true
        spacing: 4

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: facetGroup.title
                color: Theme.textMuted
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 0.7
            }

            ShadowIconButton {
                visible: facetGroup.selectedKey.length > 0
                source: "qrc:/icons/clear.svg"
                buttonSize: 24
                iconSize: 13
                toolTipText: qsTr("Clear %1 filter").arg(facetGroup.title.toLowerCase())
                accessibleName: toolTipText
                onClicked: facetGroup.controller.clearLibraryFacet(facetGroup.kind)
            }
        }

        ListView {
            id: facetList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 132)
            visible: count > 0
            clip: true
            spacing: 2
            interactive: contentHeight > height
            model: facetGroup.values

            delegate: Rectangle {
                id: facetRow
                required property var modelData
                width: facetList.width
                height: 30
                radius: Theme.compactControlRadius
                color: String(modelData.key) === facetGroup.selectedKey
                    ? Theme.accentSurfaceQuiet
                    : facetMouse.containsMouse
                        ? Theme.buttonGhostHover : Theme.transparent

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: String(facetRow.modelData.label)
                        color: String(facetRow.modelData.key)
                            === facetGroup.selectedKey
                            ? Theme.accentSelectionText : Theme.textPrimary
                        font.pixelSize: Theme.fontMeta
                        elide: Text.ElideRight
                    }

                    Label {
                        text: qsTr("%L1").arg(Number(facetRow.modelData.photoCount))
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                    }
                }

                MouseArea {
                    id: facetMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: facetGroup.controller.setLibraryFacet(
                        facetGroup.kind,
                        String(facetRow.modelData.key))
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !facetGroup.controller.libraryFacetsBusy && facetList.count === 0
            text: facetGroup.emptyText
            color: Theme.textQuiet
            font.pixelSize: Theme.fontMeta
        }
    }

    contentItem: ColumnLayout {
        id: contentColumn
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Browse Library")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
            }

            Label {
                visible: root.controller.libraryFacetsBusy
                text: qsTr("Updating…")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Use metadata facets to narrow the whole Library. The most common values are shown first.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        ScrollView {
            id: facetScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Math.min(facetColumn.implicitHeight, 620)
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: facetColumn
                width: facetScroll.availableWidth
                spacing: 10

                FacetGroup {
                    controller: root.controller
                    kind: "country"
                    title: qsTr("COUNTRIES")
                    selectedKey: root.controller.filterCountryKey
                    values: root.controller.libraryCountryFacets
                    emptyText: qsTr("No countries match the current Library view.")
                }

                FacetGroup {
                    controller: root.controller
                    kind: "city"
                    title: qsTr("CITIES")
                    selectedKey: root.controller.filterLocalityKey
                    values: root.controller.libraryCityFacets
                    emptyText: qsTr("No cities match the current Library view.")
                }

                FacetGroup {
                    controller: root.controller
                    kind: "month"
                    title: qsTr("DATES")
                    selectedKey: root.controller.filterCaptureMonth
                    values: root.controller.libraryCaptureMonthFacets
                    emptyText: qsTr("No capture dates match the current Library view.")
                }

                FacetGroup {
                    controller: root.controller
                    kind: "camera"
                    title: qsTr("CAMERAS")
                    selectedKey: root.controller.filterCameraKey
                    values: root.controller.libraryCameraFacets
                    emptyText: qsTr("No cameras match the current Library view.")
                }

                FacetGroup {
                    controller: root.controller
                    kind: "lens"
                    title: qsTr("LENSES")
                    selectedKey: root.controller.filterLensKey
                    values: root.controller.libraryLensFacets
                    emptyText: qsTr("No lenses match the current Library view.")
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }

            ShadowButton {
                compact: true
                variant: ShadowButton.Ghost
                text: qsTr("Clear all")
                visible: root.hasFacetFilter
                onClicked: {
                    root.controller.clearLibraryFacet("month")
                    root.controller.clearLibraryFacet("camera")
                    root.controller.clearLibraryFacet("lens")
                    root.controller.clearLibraryFacet("country")
                    root.controller.clearLibraryFacet("city")
                }
            }
        }
    }
}
