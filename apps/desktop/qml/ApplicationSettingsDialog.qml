pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    objectName: "applicationSettingsDialog"

    required property var preferences
    required property var aiPreferences
    required property var imageUnderstandingController
    required property var cachePreferences
    required property var cacheMaintenanceController
    required property var mapProviderPreferences
    required property var editor
    required property real hostWidth
    required property real hostHeight
    property int selectedIndex: 0

    signal openLutLibraryRequested()
    signal openOpticsProfileLibraryRequested()

    readonly property real doneButtonRightInset:
        settingsHeader.width - applicationSettingsDoneButton.x
            - applicationSettingsDoneButton.width

    readonly property var sections: [
        { key: "general", title: qsTr("General"), subtitle: qsTr("Appearance & language"), icon: "qrc:/icons/settings.svg" },
        { key: "library", title: qsTr("Library"), subtitle: qsTr("Thumbnails & metadata"), icon: "qrc:/icons/review-grid.svg" },
        { key: "ai", title: qsTr("AI & Models"), subtitle: qsTr("Local processing policy"), icon: "qrc:/icons/mask.svg" },
        { key: "storage", title: qsTr("Storage & Cache"), subtitle: qsTr("Limits & maintenance"), icon: "qrc:/icons/storage.svg" },
        { key: "maps", title: qsTr("Maps & Location"), subtitle: qsTr("Offline city data & Google"), icon: "qrc:/icons/map.svg" }
    ]

    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(920, Math.max(0, root.hostWidth - 48))
    height: Math.min(660, Math.max(0, root.hostHeight - 48))
    x: Math.round(((parent ? parent.width : root.hostWidth) - width) / 2)
    y: Math.round(((parent ? parent.height : root.hostHeight) - height) / 2)
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: mapSettingsPane.discardSecretDraft()

    function sectionIndex(sectionKey) {
        const requested = String(sectionKey || "general")
        for (let index = 0; index < sections.length; ++index) {
            if (sections[index].key === requested)
                return index
        }
        return 0
    }

    function selectSection(index) {
        selectedIndex = Math.max(0, Math.min(sections.length - 1, Number(index)))
        if (sections[selectedIndex].key === "maps")
            Qt.callLater(mapSettingsPane.prepare)
        if (sections[selectedIndex].key === "storage")
            cacheMaintenanceController.refreshInventory()
    }

    function present(sectionKey) {
        selectSection(sectionIndex(sectionKey))
        open()
    }

    background: Rectangle {
        radius: 12
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            id: settingsHeader
            Layout.fillWidth: true
            Layout.preferredHeight: 58
            color: Theme.chrome
            radius: 12

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.border
            }

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.right: applicationSettingsDoneButton.left
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1

                Label {
                    width: parent.width
                    text: qsTr("Settings")
                    color: Theme.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                Label {
                    width: parent.width
                    text: qsTr("Application, local AI, storage, and service preferences")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    elide: Text.ElideRight
                }
            }

            ShadowButton {
                id: applicationSettingsDoneButton
                objectName: "applicationSettingsDoneButton"
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                variant: ShadowButton.Primary
                text: qsTr("Done")
                onClicked: root.close()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.preferredWidth: 206
                Layout.fillHeight: true
                color: Theme.panel

                ListView {
                    id: sectionList
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 4
                    clip: true
                    model: root.sections
                    currentIndex: root.selectedIndex

                    delegate: ItemDelegate {
                        id: sectionButton
                        required property int index
                        required property var modelData
                        width: ListView.view.width
                        height: 46
                        leftPadding: 8
                        rightPadding: 8
                        topPadding: 0
                        bottomPadding: 0
                        hoverEnabled: true
                        Accessible.name: modelData.title
                        onClicked: root.selectSection(index)

                        background: Rectangle {
                            radius: Theme.controlRadius
                            color: sectionButton.index === root.selectedIndex
                                ? Theme.accentSurface
                                : sectionButton.hovered
                                    ? Theme.buttonGhostHover : Theme.transparent
                            border.width: sectionButton.visualFocus ? 1 : 0
                            border.color: Theme.focusRing
                        }

                        contentItem: RowLayout {
                            spacing: 10

                            Rectangle {
                                Layout.preferredWidth: 26
                                Layout.preferredHeight: 26
                                radius: Theme.compactControlRadius
                                color: sectionButton.index === root.selectedIndex
                                    ? Theme.accentSelectionSurface : Theme.buttonSurface

                                ShadowIcon {
                                    anchors.centerIn: parent
                                    source: sectionButton.modelData.icon
                                    color: sectionButton.index === root.selectedIndex
                                        ? Theme.accent : Theme.textSecondary
                                    size: 14
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1

                                Label {
                                    Layout.fillWidth: true
                                    text: sectionButton.modelData.title
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontSection
                                    font.weight: sectionButton.index === root.selectedIndex
                                        ? Font.DemiBold : Font.Medium
                                    elide: Text.ElideRight
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: sectionButton.modelData.subtitle
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontMeta
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: Theme.border
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 18
                currentIndex: root.selectedIndex

                SettingsGeneralPane {
                    preferences: root.preferences
                }

                SettingsLibraryPane {
                    preferences: root.preferences
                    onOpenLutLibraryRequested: root.openLutLibraryRequested()
                    onOpenOpticsProfileLibraryRequested:
                        root.openOpticsProfileLibraryRequested()
                }

                SettingsAiPane {
                    aiPreferences: root.aiPreferences
                    imageUnderstandingController: root.imageUnderstandingController
                    editor: root.editor
                }

                SettingsStoragePane {
                    cachePreferences: root.cachePreferences
                    cacheMaintenanceController: root.cacheMaintenanceController
                }

                MapProviderSettingsPane {
                    id: mapSettingsPane
                    preferences: root.mapProviderPreferences
                }
            }
        }
    }
}
