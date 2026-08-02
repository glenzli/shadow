pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var cachePreferences
    required property var cacheMaintenanceController

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
        return amount.toLocaleString(Qt.locale(), "f", fraction) + " " + units[unit]
    }

    Popup {
        id: cleanupConfirmation
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
                Layout.fillWidth: true
                text: qsTr("Remove verified unused cache files?")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Shadow will remove %1 unused file(s), reclaiming up to %2. Catalog-live, unknown, and recently published entries stay protected.")
                    .arg(Number(root.cacheMaintenanceController.plannedSweep.reclaimedBlobCount || 0))
                    .arg(root.byteText(root.cacheMaintenanceController.plannedSweep.reclaimedByteLength))
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ShadowButton {
                    text: qsTr("Cancel")
                    variant: ShadowButton.Ghost
                    onClicked: cleanupConfirmation.close()
                }
                ShadowButton {
                    text: qsTr("Remove Unused")
                    variant: ShadowButton.Danger
                    onClicked: {
                        cleanupConfirmation.close()
                        root.cacheMaintenanceController.runPlannedCleanup()
                    }
                }
            }
        }
    }

    ScrollView {
        id: storageScroll
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: storageScroll.availableWidth
            spacing: 14

            Label {
                Layout.fillWidth: true
                text: qsTr("Storage & Cache")
                color: Theme.textPrimary
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Set a local cache target and inspect what can be reclaimed without touching original photos or durable edits.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: cachePolicyContent.implicitHeight + 28
                radius: Theme.controlRadius + 2
                color: Theme.panel
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    id: cachePolicyContent
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Disk cache target")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                            font.weight: Font.DemiBold
                        }

                        ShadowCheckBox {
                            id: unlimitedCache
                            objectName: "unlimitedCacheCheckBox"
                            text: qsTr("Unlimited")
                            checked: root.cachePreferences.diskLimitGiB === 0
                            onToggled: {
                                if (checked)
                                    root.cachePreferences.diskLimitGiB = 0
                                else if (root.cachePreferences.diskLimitGiB === 0)
                                    root.cachePreferences.diskLimitGiB = 20
                            }
                        }
                    }

                    ShadowSlider {
                        objectName: "cacheLimitSlider"
                        Layout.fillWidth: true
                        label: qsTr("Target")
                        from: 1
                        to: 200
                        stepSize: 1
                        neutralValue: 20
                        fillFromMinimum: true
                        decimals: 0
                        suffix: " GiB"
                        enabled: !unlimitedCache.checked
                        value: root.cachePreferences.diskLimitGiB > 0
                            ? root.cachePreferences.diskLimitGiB : 20
                        onEdited: value =>
                            root.cachePreferences.diskLimitGiB = Math.round(value)
                    }

                    ShadowSwitch {
                        objectName: "automaticCacheCleanupSwitch"
                        Layout.fillWidth: true
                        text: qsTr("Automatically reclaim safe unused previews above the target")
                        enabled: root.cachePreferences.diskLimitGiB > 0
                        checked: root.cachePreferences.automaticCleanupAllowed
                        onToggled:
                            root.cachePreferences.automaticCleanupAllowed = checked
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("This is a safe target, not permission to delete protected data. Catalog-referenced previews, unknown entries, recent publications, and AI RAW foundations are never removed solely to meet it.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: inventoryContent.implicitHeight + 28
                radius: Theme.controlRadius + 2
                color: Theme.panel
                border.width: 1
                border.color: root.cacheMaintenanceController.overConfiguredLimit
                    ? Theme.warningText : Theme.border

                ColumnLayout {
                    id: inventoryContent
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Label {
                                text: qsTr("Preview cache")
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.DemiBold
                            }
                            Label {
                                text: root.byteText(
                                    root.cacheMaintenanceController.inventory.cacheBlobByteLength)
                                color: root.cacheMaintenanceController.overConfiguredLimit
                                    ? Theme.warningText : Theme.textSecondary
                                font.pixelSize: 18
                                font.weight: Font.DemiBold
                            }
                        }

                        BusyIndicator {
                            running: root.cacheMaintenanceController.busy
                            visible: running
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 22
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.cacheMaintenanceController.overConfiguredLimit
                            ? qsTr("Usage is above the configured target. Preview cleanup can reclaim only entries already proven safe.")
                            : qsTr("%1 cached preview file(s); %2 Catalog reference(s) remain protected.")
                                .arg(Number(root.cacheMaintenanceController.inventory.cacheBlobCount || 0))
                                .arg(Number(root.cacheMaintenanceController.inventory.catalogLiveBlobCount || 0))
                        color: root.cacheMaintenanceController.overConfiguredLimit
                            ? Theme.warningText : Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.cachePreferences.cacheRootPath
                        color: Theme.textMuted
                        font.family: "Menlo"
                        font.pixelSize: Theme.fontMeta
                        elide: Text.ElideMiddle
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        ShadowButton {
                            text: qsTr("Refresh")
                            enabled: !root.cacheMaintenanceController.busy
                            onClicked: root.cacheMaintenanceController.refreshInventory()
                        }

                        ShadowButton {
                            text: qsTr("Preview Cleanup")
                            enabled: !root.cacheMaintenanceController.busy
                            onClicked: root.cacheMaintenanceController.planSafeCleanup()
                        }

                        ShadowButton {
                            text: qsTr("Remove Unused…")
                            variant: ShadowButton.Danger
                            enabled: root.cacheMaintenanceController.hasPlan
                                && !root.cacheMaintenanceController.busy
                                && Number(root.cacheMaintenanceController.plannedSweep.reclaimedBlobCount || 0) > 0
                            onClicked: cleanupConfirmation.open()
                        }

                        Item { Layout.fillWidth: true }

                        ShadowButton {
                            text: qsTr("Show Cache Folder")
                            variant: ShadowButton.Ghost
                            onClicked: Qt.openUrlExternally(root.cachePreferences.cacheRootUrl)
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.cacheMaintenanceController.statusText.length > 0
                        text: root.cacheMaintenanceController.statusText
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.cacheMaintenanceController.errorText.length > 0
                        text: root.cacheMaintenanceController.errorText
                        color: Theme.errorText
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
