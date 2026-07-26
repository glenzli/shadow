pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "cacheMaintenanceWindow"

    required property var cacheMaintenanceController

    width: 680
    height: 560
    minimumWidth: 560
    minimumHeight: 460
    title: qsTr("Cache Maintenance")
    visible: false
    modality: Qt.NonModal
    color: Theme.window
    palette.window: Theme.window
    palette.windowText: Theme.textPrimary
    palette.base: Theme.panelRaised
    palette.text: Theme.textPrimary
    palette.button: Theme.buttonSurface
    palette.buttonText: Theme.textPrimary
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.selectionForeground

    function byteText(value) {
        const bytes = Math.max(0, Number(value || 0))
        const units = ["B", "KB", "MB", "GB", "TB"]
        let amount = bytes
        let unit = 0
        while (amount >= 1024 && unit < units.length - 1) {
            amount /= 1024
            unit += 1
        }
        const fraction = unit === 0 || amount >= 100 ? 0 : amount >= 10 ? 1 : 2
        return amount.toLocaleString(Qt.locale(), 'f', fraction) + " " + units[unit]
    }

    function countText(value) {
        return Number(value || 0).toLocaleString(Qt.locale(), 'f', 0)
    }

    function present() {
        show()
        raise()
        requestActivate()
        cacheMaintenanceController.refreshInventory()
    }

    header: ToolBar {
        implicitHeight: 58
        background: Rectangle {
            color: Theme.chrome
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.border
            }
        }
        contentItem: RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 14
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: qsTr("CACHE MAINTENANCE")
                    color: Theme.textPrimary
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.9
                }
                Label {
                    text: qsTr("Preview first. Remove only verified unused cache files.")
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
            }

            ShadowIconButton {
                source: "qrc:/icons/redo.svg"
                toolTipText: qsTr("Refresh cache usage")
                accessibleName: toolTipText
                enabled: !root.cacheMaintenanceController.busy
                onClicked: root.cacheMaintenanceController.refreshInventory()
            }
        }
    }

    Popup {
        id: confirmationPopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(430, parent.width - 40)
        padding: 18
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: Theme.controlRadius + 2
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            spacing: 12
            Label {
                text: qsTr("REMOVE UNUSED CACHE FILES?")
                color: Theme.textPrimary
                font.pixelSize: 12
                font.weight: Font.DemiBold
                font.letterSpacing: 0.7
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow will remove only %1 verified unused cache file(s), up to %2. Catalog-live entries, unknown entries, and recently published previews stay protected. Safety is checked once more immediately before removal.")
                    .arg(root.countText(root.cacheMaintenanceController.plannedSweep.reclaimedBlobCount))
                    .arg(root.byteText(root.cacheMaintenanceController.plannedSweep.reclaimedByteLength))
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                lineHeight: 1.28
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ShadowButton {
                    text: qsTr("CANCEL")
                    variant: ShadowButton.Ghost
                    onClicked: confirmationPopup.close()
                }
                ShadowButton {
                    text: qsTr("REMOVE UNUSED")
                    variant: ShadowButton.Danger
                    onClicked: {
                        confirmationPopup.close()
                        root.cacheMaintenanceController.runPlannedCleanup()
                    }
                }
            }
        }
    }

    ScrollView {
        anchors.fill: parent
        clip: true

        ColumnLayout {
            width: Math.min(620, parent.width - 40)
            anchors.horizontalCenter: parent.horizontalCenter
            topPadding: 20
            bottomPadding: 24
            spacing: 14

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: safetyColumn.implicitHeight + 26
                radius: Theme.controlRadius
                color: Theme.accentSurfaceQuiet
                border.width: 1
                border.color: Theme.accentBorder

                ColumnLayout {
                    id: safetyColumn
                    anchors.fill: parent
                    anchors.margins: 13
                    spacing: 4
                    Label {
                        text: qsTr("SAFE BY DESIGN")
                        color: Theme.accent
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("The catalog is the authority. This tool never deletes active previews, unknown entries, or newly published cache files.")
                        color: Theme.textPrimary
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width >= 600 ? 3 : 1
                columnSpacing: 10
                rowSpacing: 10

                Repeater {
                    model: [
                        { title: qsTr("PREVIEW CACHE"), value: root.byteText(root.cacheMaintenanceController.inventory.cacheBlobByteLength), detail: qsTr("%1 cached blob(s)").arg(root.countText(root.cacheMaintenanceController.inventory.cacheBlobCount)) },
                        { title: qsTr("CATALOG PROTECTED"), value: root.countText(root.cacheMaintenanceController.inventory.catalogLiveBlobCount), detail: qsTr("live blob reference(s)") },
                        { title: qsTr("HELD FOR REVIEW"), value: root.countText(root.cacheMaintenanceController.inventory.unknownEntryCount), detail: qsTr("%1 unsupported algorithm(s)").arg(root.countText(root.cacheMaintenanceController.inventory.unsupportedAlgorithmCount)) }
                    ]

                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 88
                        radius: Theme.controlRadius
                        color: Theme.panelRaised
                        border.width: 1
                        border.color: Theme.border

                        Column {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 4
                            Label {
                                text: modelData.title
                                color: Theme.textMuted
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.65
                            }
                            Label {
                                text: modelData.value
                                color: Theme.textPrimary
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Label {
                                width: parent.width
                                text: modelData.detail
                                color: Theme.textMuted
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.cacheMaintenanceController.statusText.length > 0
                text: root.cacheMaintenanceController.statusText
                color: Theme.textMuted
                font.pixelSize: 11
                horizontalAlignment: Text.AlignHCenter
            }

            Label {
                Layout.fillWidth: true
                visible: root.cacheMaintenanceController.errorText.length > 0
                text: root.cacheMaintenanceController.errorText
                color: Theme.errorText
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: cleanupColumn.implicitHeight + 28
                radius: Theme.controlRadius
                color: Theme.panel
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    id: cleanupColumn
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("SAFE CLEANUP PREVIEW")
                            color: Theme.textPrimary
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.6
                        }
                        BusyIndicator {
                            running: root.cacheMaintenanceController.busy
                            visible: running
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 18
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.cacheMaintenanceController.hasPlan
                            ? qsTr("%1 unused blob(s) can be removed, reclaiming up to %2. %3 recently published blob(s) remain protected.")
                                .arg(root.countText(root.cacheMaintenanceController.plannedSweep.reclaimedBlobCount))
                                .arg(root.byteText(root.cacheMaintenanceController.plannedSweep.reclaimedByteLength))
                                .arg(root.countText(root.cacheMaintenanceController.plannedSweep.recentlyProtectedBlobCount))
                            : qsTr("Create a preview before removing anything. The preview performs no filesystem mutation.")
                        color: Theme.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        ShadowButton {
                            text: qsTr("PREVIEW CLEANUP")
                            variant: ShadowButton.Secondary
                            enabled: !root.cacheMaintenanceController.busy
                            onClicked: root.cacheMaintenanceController.planSafeCleanup()
                        }
                        ShadowButton {
                            text: qsTr("REMOVE UNUSED…")
                            variant: ShadowButton.Danger
                            enabled: root.cacheMaintenanceController.hasPlan
                                && !root.cacheMaintenanceController.busy
                                && Number(root.cacheMaintenanceController.plannedSweep.reclaimedBlobCount) > 0
                            onClicked: confirmationPopup.open()
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                visible: !root.cacheMaintenanceController.completedSweep.dryRun
                    && root.cacheMaintenanceController.completedSweep.reclaimedBlobCount !== undefined
                implicitHeight: completionLabel.implicitHeight + 24
                radius: Theme.controlRadius
                color: Theme.successSurface
                border.width: 1
                border.color: Theme.successBorder

                Label {
                    id: completionLabel
                    anchors.fill: parent
                    anchors.margins: 12
                    text: qsTr("Removed %1 cache file(s), reclaiming %2.")
                        .arg(root.countText(root.cacheMaintenanceController.completedSweep.reclaimedBlobCount))
                        .arg(root.byteText(root.cacheMaintenanceController.completedSweep.reclaimedByteLength))
                    color: Theme.successText
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
