pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: library

    required property var controller
    signal chooseFolderRequested()

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
            width: Math.min(760, parent.width - 64)
            x: Math.round((parent.width - width) / 2)
            spacing: 18

            Item { Layout.preferredHeight: 32 }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    radius: 8
                    color: Theme.accentSurface

                    ShadowIcon {
                        anchors.centerIn: parent
                        source: "qrc:/icons/library-manage.svg"
                        color: Theme.accent
                        size: 20
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Library Management")
                        color: Theme.textPrimary
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Organize photos and manage local sources. Original files remain read-only.")
                        color: Theme.textMuted
                        font.pixelSize: 11
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            LibraryKeywordPanel {
                Layout.fillWidth: true
                Layout.preferredHeight: 470
                controller: library.controller
                allowAssignment: false
                allowFiltering: true
                manageTaxonomy: true
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            LibrarySourceHealthPane {
                Layout.fillWidth: true
                controller: library.controller
            }

            LibraryImportPane {
                Layout.fillWidth: true
                controller: library.controller
                onChooseFolderRequested: library.chooseFolderRequested()
            }

            Item { Layout.preferredHeight: 32 }
        }
    }
}
