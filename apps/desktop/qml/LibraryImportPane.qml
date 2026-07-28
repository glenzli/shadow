pragma ComponentBehavior: Bound
pragma Translator: "LibraryWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns catalog count, source-folder admission, and observable import activity.
ColumnLayout {
    id: importPane

    required property var controller
    signal chooseFolderRequested()

    readonly property bool activityRunning: controller.scanning
        || controller.refreshing || controller.busy

    spacing: 18

    RowLayout {
        Layout.fillWidth: true

        Label {
            text: qsTr("CATALOG")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            font.weight: Font.DemiBold
            font.letterSpacing: 0.5
        }

        Item { Layout.fillWidth: true }

        Label {
            text: qsTr("%L1 photos").arg(importPane.controller.itemCount)
            color: Theme.textSecondary
            font.pixelSize: Theme.fontMeta
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: sourceContent.implicitHeight + 32
        radius: 8
        color: Theme.panelRaised
        border.color: Theme.border

        RowLayout {
            id: sourceContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 16
            anchors.rightMargin: 12
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 36
                Layout.preferredHeight: 36
                radius: Theme.controlRadius
                color: Theme.surfaceSubtle

                ShadowIcon {
                    anchors.centerIn: parent
                    source: "qrc:/icons/add-folder.svg"
                    color: Theme.textMuted
                    size: 18
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3

                Label {
                    Layout.fillWidth: true
                    text: qsTr("IMPORT FROM FOLDER")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSection
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    text: importPane.controller.folderPath.length > 0
                        ? importPane.controller.folderPath
                        : qsTr("Choose a folder to add photos")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    elide: Text.ElideMiddle
                }
            }

            ShadowIconButton {
                source: "qrc:/icons/add-folder.svg"
                variant: ShadowIconButton.Secondary
                toolTipText: importPane.controller.folderPath.length > 0
                    ? qsTr("CHOOSE ANOTHER FOLDER")
                    : qsTr("ADD PHOTO FOLDER")
                accessibleName: toolTipText
                enabled: !importPane.controller.scanning
                    && !importPane.controller.refreshing
                    && !importPane.controller.busy
                    && !importPane.controller.loadingMore
                    && !importPane.controller.comparisonBusy
                    && !importPane.controller.decisionBusy
                onClicked: importPane.chooseFolderRequested()
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: activityContent.implicitHeight + 28
        visible: importPane.activityRunning
            || Number(importPane.controller.scanProgress.scanId) > 0
        radius: 8
        color: Theme.panel
        border.color: Theme.border

        ColumnLayout {
            id: activityContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 8

            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("LIBRARY ACTIVITY")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontMeta
                    font.weight: Font.DemiBold
                }

                BusyIndicator {
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    visible: importPane.activityRunning
                    running: visible
                }
            }

            Label {
                Layout.fillWidth: true
                text: importPane.controller.statusText
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideRight
            }

            ProgressBar {
                Layout.fillWidth: true
                visible: importPane.activityRunning
                indeterminate: true
            }

            RowLayout {
                Layout.fillWidth: true
                visible: importPane.controller.scanning

                Item { Layout.fillWidth: true }

                ShadowIconButton {
                    source: "qrc:/icons/clear.svg"
                    variant: ShadowIconButton.Danger
                    toolTipText: qsTr("STOP IMPORT")
                    accessibleName: toolTipText
                    enabled: importPane.controller.scanProgress.phase
                        !== "cancelling"
                    onClicked: importPane.controller.cancelScan()
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Shadow keeps catalog decisions, previews and edit history in its local Library while source photos stay untouched.")
        color: Theme.textSubtle
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.WordWrap
        lineHeight: 1.35
    }

}
