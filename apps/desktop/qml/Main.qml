pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property var controller
    required property var editor
    property int workspaceIndex: 0

    width: 1480
    height: 920
    minimumWidth: 1080
    minimumHeight: 680
    visible: true
    color: "#0c0e10"
    title: workspaceIndex === 0 ? "Shadow · Review" : "Shadow · Precision"

    readonly property color panel: "#121519"
    readonly property color panelRaised: "#181c21"
    readonly property color border: "#2a3037"
    readonly property color textPrimary: "#edf0f2"
    readonly property color textMuted: "#8b949e"
    readonly property color accent: "#d8b36a"

    function showReview() {
        workspaceIndex = 0
    }

    function showPrecision() {
        if (editor.active || editor.busy)
            workspaceIndex = 1
    }

    function openPrecision(photoId, representationId, sourcePath, photoTitle) {
        editor.openPhoto(photoId, representationId, sourcePath, photoTitle)
        workspaceIndex = 1
    }

    onClosing: close => {
        if (editor.dirty) {
            editor.closePhoto()
            workspaceIndex = 1
            close.accepted = false
        }
    }

    header: Rectangle {
        height: 58
        color: "#101317"
        border.color: window.border

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 16
            spacing: 14

            Label {
                text: "SHADOW"
                color: window.textPrimary
                font.pixelSize: 17
                font.weight: Font.DemiBold
                font.letterSpacing: 3.2
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 24
                color: window.border
            }

            Button {
                id: reviewModeButton
                Layout.preferredWidth: 92
                Layout.preferredHeight: 40
                text: "REVIEW"
                flat: true
                onClicked: window.showReview()

                background: Item {
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 2
                        color: window.workspaceIndex === 0 ? window.accent : "transparent"
                    }
                }
                contentItem: Label {
                    text: reviewModeButton.text
                    color: window.workspaceIndex === 0 ? window.accent : window.textMuted
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.4
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Button {
                id: precisionModeButton
                Layout.preferredWidth: 108
                Layout.preferredHeight: 40
                text: "PRECISION"
                flat: true
                enabled: window.editor.active || window.editor.busy
                onClicked: window.showPrecision()

                background: Item {
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 2
                        color: window.workspaceIndex === 1 ? window.accent : "transparent"
                    }
                }
                contentItem: Label {
                    text: precisionModeButton.text
                    color: !precisionModeButton.enabled
                        ? "#4b535c"
                        : window.workspaceIndex === 1 ? window.accent : window.textMuted
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.4
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Label {
                Layout.fillWidth: true
                text: window.workspaceIndex === 0
                    ? (window.controller.folderPath.length > 0
                        ? window.controller.folderPath
                        : "No library folder selected")
                    : (window.editor.sourcePath.length > 0
                        ? window.editor.sourcePath
                        : "Preparing local edit session")
                color: window.textMuted
                elide: Text.ElideMiddle
                horizontalAlignment: Text.AlignHCenter
            }

            Rectangle {
                visible: window.workspaceIndex === 1 && window.editor.active
                Layout.preferredWidth: dirtyLabel.implicitWidth + 20
                Layout.preferredHeight: 26
                radius: 13
                color: window.editor.dirty ? "#30291d" : "#19241f"
                border.color: window.editor.dirty ? "#5d4b2d" : "#294436"

                Label {
                    id: dirtyLabel
                    anchors.centerIn: parent
                    text: window.editor.dirty ? "UNSAVED" : "SAVED"
                    color: window.editor.dirty ? window.accent : "#91bda0"
                    font.pixelSize: 9
                    font.weight: Font.Bold
                    font.letterSpacing: 0.8
                }
            }

            Button {
                id: folderButton
                visible: window.workspaceIndex === 0
                text: window.controller.busy
                    ? "SCANNING…"
                    : window.controller.comparisonBusy ? "RECORDING…"
                    : window.controller.decisionBusy ? "SAVING…" : "CHOOSE FOLDER"
                enabled: !window.controller.busy && !window.controller.loadingMore
                    && !window.controller.comparisonBusy
                    && !window.controller.decisionBusy
                onClicked: reviewWorkspace.chooseFolder()

                background: Rectangle {
                    radius: 4
                    color: folderButton.down ? "#b9914e" : window.accent
                }
                contentItem: Label {
                    text: folderButton.text
                    color: "#17130d"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Button {
                id: closeEditButton
                visible: window.workspaceIndex === 1
                text: "CLOSE EDIT"
                enabled: !window.editor.stateBusy
                onClicked: {
                    window.editor.closePhoto()
                    if (!window.editor.active)
                        window.showReview()
                }

                background: Rectangle {
                    radius: 4
                    color: closeEditButton.down ? "#272d33" : window.panelRaised
                    border.color: window.border
                }
                contentItem: Label {
                    text: closeEditButton.text
                    color: window.textPrimary
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: window.workspaceIndex

        ReviewWorkspace {
            id: reviewWorkspace
            Layout.fillWidth: true
            Layout.fillHeight: true
            controller: window.controller
            onOpenPrecisionRequested: (photoId, representationId, sourcePath, photoTitle) => {
                window.openPrecision(photoId, representationId, sourcePath, photoTitle)
            }
        }

        PrecisionWorkspace {
            Layout.fillWidth: true
            Layout.fillHeight: true
            editor: window.editor
        }
    }

    footer: Rectangle {
        height: 30
        color: "#101317"
        border.color: window.border

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 10

            BusyIndicator {
                Layout.preferredWidth: 15
                Layout.preferredHeight: 15
                visible: window.workspaceIndex === 0
                    ? window.controller.busy || window.controller.loadingMore
                        || window.controller.comparisonBusy
                        || window.controller.decisionBusy
                    : window.editor.busy
                running: visible
            }

            Label {
                Layout.fillWidth: true
                text: window.workspaceIndex === 0
                    ? (window.controller.decisionBusy
                        ? window.controller.decisionStatusText
                        : window.controller.comparisonBusy
                        ? window.controller.comparisonStatusText
                        : window.controller.statusText)
                    : window.editor.statusText
                color: window.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
            }
            Label {
                text: window.workspaceIndex === 0 ? "REVIEW" : "PRECISION"
                color: "#56616b"
                font.pixelSize: 9
                font.letterSpacing: 0.8
            }
        }
    }
}
