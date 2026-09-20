pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property var libraryServerController
    required property bool initialDarkAppearance

    visible: true
    width: 780
    height: 760
    minimumWidth: 620
    minimumHeight: 560
    title: qsTr("Shadow Server")
    color: Theme.window

    Component.onCompleted: {
        Theme.effectiveDark = initialDarkAppearance;
        libraryServerController.refresh();
    }

    onClosing: close => {
        if (libraryServerController.running) {
            close.accepted = false;
            window.showMinimized();
        }
    }

    header: Rectangle {
        implicitHeight: 66
        color: Theme.chrome

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.border
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 22
            anchors.rightMargin: 22
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: Theme.controlRadius + 2
                color: Theme.accentSurface

                Label {
                    anchors.centerIn: parent
                    text: "S"
                    color: Theme.accent
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.Bold
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Shadow Server")
                    color: Theme.textPrimary
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    text: window.libraryServerController.running ? qsTr("Serving this Mac's photo library") : qsTr("Local Library server controller")
                    color: window.libraryServerController.running ? Theme.successText : Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }
            }

            Rectangle {
                Layout.preferredWidth: 9
                Layout.preferredHeight: 9
                radius: 5
                color: window.libraryServerController.running ? Theme.successText : Theme.textMuted
            }
        }
    }

    SettingsLibraryServerPane {
        anchors.fill: parent
        anchors.leftMargin: 22
        anchors.rightMargin: 22
        anchors.topMargin: 18
        anchors.bottomMargin: 18
        controller: window.libraryServerController
        autoStartLabel: qsTr("Start when Shadow Server opens")
    }
}
