pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Popup {
    id: dialog

    required property var exportController
    property var targets: []

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(900, parent.width - 40)
    height: Math.min(760, parent.height - 40)
    padding: 0
    modal: true
    dim: true
    focus: true
    // Closing only hides this surface. The controller owns the durable job.
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function present(exportTargets) {
        if (!exportController.busy) {
            targets = exportTargets || [];
            presetSidebar.resetSelection();
        }
        open();
    }

    background: Rectangle {
        radius: Theme.controlRadius + 2
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    FolderDialog {
        id: destinationFolderDialog
        title: qsTr("Choose export folder")
        onAccepted: dialog.exportController.startExport(dialog.targets, selectedFolder, exportSettings.options())
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 12
            Layout.topMargin: 15
            Layout.bottomMargin: 13
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: qsTr("EXPORT")
                    color: Theme.textPrimary
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.0
                }
                Label {
                    text: qsTr("%L1 selected photos").arg(dialog.exportController.busy
                        ? dialog.exportController.totalCount : dialog.targets.length)
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
                Label {
                    text: qsTr("Saved locally · unfinished exports resume automatically")
                    color: Theme.textMuted
                    font.pixelSize: 9
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            ShadowIconButton {
                source: "qrc:/icons/clear.svg"
                toolTipText: qsTr("Close")
                accessibleName: toolTipText
                onClicked: dialog.close()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            ExportPresetSidebar {
                id: presetSidebar
                Layout.preferredWidth: 220
                Layout.fillHeight: true
                exportController: dialog.exportController
                optionProvider: function() { return exportSettings.options() }
                onPresetActivated: preset => exportSettings.applyPreset(preset)
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: Theme.border
            }

            ExportSettingsPane {
                id: exportSettings
                Layout.fillWidth: true
                Layout.fillHeight: true
                exportController: dialog.exportController
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 10
            Layout.bottomMargin: 10
            Layout.preferredHeight: Math.min(
                                      132,
                                      80 + Math.max(0, dialog.exportController.errors.length - 1) * 26
                                  )
            visible: !dialog.exportController.busy
                     && dialog.exportController.errors.length > 0
            radius: Theme.compactControlRadius
            color: Theme.dangerSurface
            border.width: 1
            border.color: Theme.errorBorder

            ColumnLayout {
                id: errorDetails
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6

                Label {
                    text: qsTr("FAILED ITEMS · %1").arg(dialog.exportController.errors.length)
                    color: Theme.dangerText
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.55
                }

                ListView {
                    id: errorList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 42
                    clip: true
                    spacing: 4
                    model: dialog.exportController.errors
                    delegate: Label {
                        required property string modelData
                        width: errorList.width
                        text: modelData
                        color: Theme.textSecondary
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 12
            Layout.bottomMargin: 12
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    BusyIndicator {
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        visible: dialog.exportController.busy
                        running: visible
                    }

                    Label {
                        Layout.fillWidth: true
                        text: dialog.exportController.statusText
                        color: dialog.exportController.errors.length > 0
                               && !dialog.exportController.busy
                               ? Theme.errorText : Theme.textMuted
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                }

                ProgressBar {
                    id: exportProgress
                    Layout.fillWidth: true
                    Layout.preferredHeight: 4
                    visible: dialog.exportController.busy
                    from: 0
                    to: Math.max(1, dialog.exportController.totalCount)
                    value: Math.min(
                               dialog.exportController.currentCount,
                               dialog.exportController.totalCount
                           )
                    background: Rectangle {
                        radius: height / 2
                        color: Theme.controlQuiet
                    }
                    contentItem: Item {
                        Rectangle {
                            width: parent.width * exportProgress.visualPosition
                            height: parent.height
                            radius: height / 2
                            color: dialog.exportController.cancellationRequested
                                   ? Theme.textMuted : Theme.accent
                        }
                    }
                }
            }

            ShadowButton {
                objectName: "exportBackgroundButton"
                visible: dialog.exportController.busy
                text: qsTr("CONTINUE EDITING")
                variant: ShadowButton.Ghost
                onClicked: dialog.close()
            }

            ShadowButton {
                objectName: "exportCancelButton"
                text: qsTr("CANCEL")
                variant: ShadowButton.Ghost
                enabled: !dialog.exportController.cancellationRequested
                onClicked: {
                    if (dialog.exportController.busy)
                        dialog.exportController.cancelExport();
                    else
                        dialog.close();
                }
            }

            ShadowButton {
                text: dialog.exportController.busy ? qsTr("EXPORTING…") : qsTr("EXPORT")
                variant: ShadowButton.Primary
                enabled: !dialog.exportController.busy && dialog.targets.length > 0
                onClicked: destinationFolderDialog.open()
            }
        }
    }

}
