pragma ComponentBehavior: Bound
pragma Translator: "XmpImportDialog"

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ShadowDialog {
    id: dialog

    required property var interchangeController
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(620, Math.max(0, parent.width - 48))
    height: Math.min(690, Math.max(0, parent.height - 48))
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 0

    function chooseFile() {
        interchangeController.clear()
        xmpFileDialog.open()
    }

    background: Rectangle {
        radius: 10
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    header: Rectangle {
        implicitHeight: 58
        color: Theme.transparent

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 12
            spacing: 8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: qsTr("Import XMP adjustments")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.interchangeController.sourceName
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    elide: Text.ElideMiddle
                }
            }
            ShadowIconButton {
                source: "qrc:/icons/close.svg"
                toolTipText: qsTr("Close")
                accessibleName: qsTr("Close XMP import")
                onClicked: dialog.close()
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.border
        }
    }

    contentItem: ScrollView {
        id: previewScroll
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: previewScroll.availableWidth
            spacing: 14
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.topMargin: 16
                text: qsTr("Shadow maps only adjustments with a stable local equivalent. The values below are approximate because the source and Shadow use different processing pipelines.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSection
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                visible: dialog.interchangeController.compatibilityWarnings.length > 0
                text: dialog.interchangeController.compatibilityWarnings.join("\n")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSection
                wrapMode: Text.Wrap
            }
            Rectangle {
                visible: dialog.interchangeController.processVersion.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                implicitHeight: 34
                radius: 6
                color: Theme.panel
                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("Camera Raw process version: %1")
                        .arg(dialog.interchangeController.processVersion)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }
            }
            Label {
                visible: dialog.interchangeController.mappedAdjustments.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                text: qsTr("WILL CREATE ONE NEW GRADE NODE · %1 ADJUSTMENTS")
                    .arg(dialog.interchangeController.mappedAdjustments.length)
                color: Theme.textMuted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                font.letterSpacing: 0.35
            }
            Repeater {
                model: dialog.interchangeController.mappedAdjustments
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.leftMargin: 20
                    Layout.rightMargin: 20
                    implicitHeight: 46
                    radius: 6
                    color: Theme.panel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Label {
                                text: modelData.targetName
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontSection
                                font.weight: Font.Medium
                            }
                            Label {
                                text: modelData.sourceName + " · " + modelData.sourceValue
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                        Label {
                            text: modelData.targetValue
                            color: Theme.accent
                            font.pixelSize: Theme.fontSection
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
            Rectangle {
                visible: dialog.interchangeController.errorText.length > 0
                    || dialog.interchangeController.invalidFields.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                implicitHeight: invalidColumn.implicitHeight + 20
                radius: 6
                color: Theme.dangerSurface
                border.width: 1
                border.color: Theme.errorBorder
                ColumnLayout {
                    id: invalidColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 5
                    Label {
                        visible: dialog.interchangeController.errorText.length > 0
                        Layout.fillWidth: true
                        text: dialog.interchangeController.errorText
                        color: Theme.errorText
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: dialog.interchangeController.invalidFields
                        delegate: Label {
                            required property var modelData
                            Layout.fillWidth: true
                            text: modelData.sourceName + " · "
                                + modelData.sourceValue + " · " + modelData.problem
                            color: Theme.errorText
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
            ColumnLayout {
                visible: dialog.interchangeController.ignoredFields.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                spacing: 6
                Label {
                    text: qsTr("NOT IMPORTED · %1 FIELDS")
                        .arg(dialog.interchangeController.ignoredFields.length)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.35
                }
                Repeater {
                    model: dialog.interchangeController.ignoredFields
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            text: modelData.sourceName
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontMeta
                            font.weight: Font.Medium
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.reason
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
            Label {
                visible: dialog.interchangeController.applyErrorText.length > 0
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.bottomMargin: 16
                text: dialog.interchangeController.applyErrorText
                color: Theme.errorText
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.Wrap
            }
        }
    }

    footer: Rectangle {
        implicitHeight: 58
        color: Theme.transparent
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.border
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 8
            Item { Layout.fillWidth: true }
            ShadowButton {
                text: qsTr("Cancel")
                variant: ShadowButton.Secondary
                onClicked: dialog.close()
            }
            ShadowButton {
                text: qsTr("Import as new node")
                variant: ShadowButton.Primary
                enabled: dialog.interchangeController.canApply
                onClicked: {
                    if (dialog.interchangeController.applyXmp())
                        dialog.close()
                }
            }
        }
    }

    FileDialog {
        id: xmpFileDialog
        title: qsTr("Choose an XMP sidecar")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("XMP sidecars (*.xmp)"), qsTr("All files (*)")]
        onAccepted: {
            dialog.interchangeController.previewXmp(selectedFile)
            dialog.open()
        }
    }
}
