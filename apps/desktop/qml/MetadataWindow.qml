pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: root

    required property var preferences
    property string photoTitle: ""
    property string sourcePath: ""
    property bool hasMetadata: false
    property bool metadataPending: false
    property var fields: []

    title: qsTr("Photo Metadata")
    width: 620
    height: 680
    minimumWidth: 480
    minimumHeight: 480
    color: Theme.window
    flags: Qt.Window

    function present() {
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
                text: root.metadataPending
                    ? qsTr("Metadata is being prepared")
                    : qsTr("No decoded metadata is available for this photo")
                color: Theme.textMuted
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
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

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 46
                                radius: 8
                                color: fieldMouse.containsMouse
                                    ? Theme.buttonGhostHover : Theme.panelRaised
                                border.width: 1
                                border.color: Theme.border

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 10
                                    spacing: 12

                                    Label {
                                        Layout.preferredWidth: 150
                                        text: fieldDelegate.modelData.label
                                        color: Theme.textMuted
                                        font.pixelSize: 10
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: fieldDelegate.modelData.value
                                        color: Theme.textPrimary
                                        font.pixelSize: 11
                                        elide: Text.ElideMiddle
                                    }

                                    Rectangle {
                                        id: sidebarToggle
                                        readonly property bool checked:
                                            root.preferences.exifFields.indexOf(
                                                fieldDelegate.modelData.id) >= 0
                                        Layout.preferredWidth: 24
                                        Layout.preferredHeight: 24
                                        radius: 6
                                        color: checked
                                            ? Theme.accentSelectionSurface
                                            : Theme.control
                                        border.width: 1
                                        border.color: checked
                                            ? Theme.accentBorder : Theme.borderStrong

                                        Accessible.role: Accessible.CheckBox
                                        Accessible.name: qsTr("Show %1 in sidebar").arg(
                                            fieldDelegate.modelData.label)
                                        Accessible.checked: checked

                                        ShadowIcon {
                                            anchors.centerIn: parent
                                            visible: sidebarToggle.checked
                                            source: "qrc:/icons/check.svg"
                                            color: Theme.accent
                                            size: 15
                                        }
                                    }
                                }

                                MouseArea {
                                    id: fieldMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.preferences.setExifFieldVisible(
                                        fieldDelegate.modelData.id,
                                        !sidebarToggle.checked)
                                }
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
        }
    }
}
