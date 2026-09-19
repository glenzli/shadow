pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root

    required property var preferences
    required property var controller
    property string photoTitle: ""
    property string photoId: ""
    property var selectionTargets: []
    property string sourcePath: ""
    property bool hasMetadata: false
    property bool metadataPending: false
    property bool metadataFailed: false
    property var fields: []
    signal retryRequested()

    title: qsTr("Photo Metadata")
    width: 620
    height: 680
    minimumWidth: 480
    minimumHeight: 480
    color: Theme.window
    flags: Qt.Window

    Shortcut {
        sequences: [StandardKey.Close]
        context: Qt.WindowShortcut
        enabled: root.visible
        onActivated: root.close()
    }

    function present() {
        if (root.photoId.length > 0)
            root.controller.requestLibraryMetadata(root.photoId)
        show()
        raise()
        requestActivate()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.window

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3

                    Label {
                        Layout.fillWidth: true
                        text: root.photoTitle.length > 0
                            ? root.photoTitle : qsTr("No photo selected")
                        color: Theme.textPrimary
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                        elide: Text.ElideMiddle
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.sourcePath
                        visible: text.length > 0
                        color: Theme.textMuted
                        font.pixelSize: 10
                        elide: Text.ElideMiddle
                    }
                }

                ColumnLayout {
                    spacing: 2

                    Label {
                        text: qsTr("SIDEBAR")
                        color: Theme.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Label {
                        text: qsTr("Pin the fields you want to see in the library inspector")
                        color: Theme.textQuiet
                        font.pixelSize: 9
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            Label {
                Layout.fillWidth: true
                visible: !root.hasMetadata
                text: root.metadataFailed
                    ? qsTr("Could not load metadata for this photo")
                    : root.metadataPending
                        ? qsTr("Metadata is being prepared")
                        : qsTr("No decoded metadata is available for this photo")
                color: Theme.textMuted
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            ShadowButton {
                Layout.alignment: Qt.AlignHCenter
                visible: !root.hasMetadata && root.metadataFailed
                text: qsTr("Retry")
                variant: ShadowButton.Ghost
                enabled: !root.metadataPending
                onClicked: root.retryRequested()
            }

            ScrollView {
                id: metadataScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.hasMetadata
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: metadataScroll.availableWidth
                    spacing: 7

                    Repeater {
                        model: root.fields

                        delegate: ColumnLayout {
                            id: fieldDelegate
                            required property int index
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 6

                            Label {
                                Layout.fillWidth: true
                                Layout.topMargin: fieldDelegate.index === 0 ? 0 : 8
                                visible: fieldDelegate.modelData.firstInGroup
                                text: fieldDelegate.modelData.group
                                color: Theme.textSecondary
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }

                            MetadataFieldSelectorRow {
                                Layout.fillWidth: true
                                fieldLabel: fieldDelegate.modelData.label
                                fieldValue: fieldDelegate.modelData.value
                                checked: root.preferences.exifFields.indexOf(
                                    fieldDelegate.modelData.id) >= 0
                                onToggleRequested: checked =>
                                    root.preferences.setExifFieldVisible(
                                        fieldDelegate.modelData.id, checked)
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.hasMetadata

                Label {
                    Layout.fillWidth: true
                    text: qsTr("All currently decoded fields are shown here.")
                    color: Theme.textQuiet
                    font.pixelSize: 9
                }

                ShadowButton {
                    text: qsTr("RESET SIDEBAR FIELDS")
                    variant: ShadowButton.Ghost
                    onClicked: root.preferences.resetExifFields()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.photoId.length > 0

                ShadowButton {
                    text: qsTr("EDIT TIME & LOCATION")
                    variant: ShadowButton.Ghost
                    onClicked: metadataEditor.present(root.photoId)
                }

                ShadowButton {
                    text: qsTr("BATCH TIME")
                    variant: ShadowButton.Ghost
                    enabled: root.selectionTargets.length > 0
                    onClicked: captureTimeBatch.present(
                        root.selectionTargets)
                }

                ShadowButton {
                    text: qsTr("IMPORT GPX")
                    variant: ShadowButton.Ghost
                    onClicked: gpxImport.present(root.selectionTargets)
                }
            }
        }
    }

    LibraryMetadataEditor {
        id: metadataEditor
        transientParent: root
        controller: root.controller
    }

    LibraryCaptureTimeBatchDialog {
        id: captureTimeBatch
        transientParent: root
        controller: root.controller
    }

    LibraryGpxImportDialog {
        id: gpxImport
        transientParent: root
        controller: root.controller
    }
}
