pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Entry and lifecycle controls for one bounded, session-only grouping job.
Item {
    id: control
    required property var workspace
    implicitWidth: 32
    implicitHeight: 28

    ShadowIconButton {
        anchors.centerIn: parent
        source: "qrc:/icons/source-stack.svg"
        selected: control.workspace.reviewClusteringController.hasResults
        toolTipText: qsTr("Group similar review candidates")
        accessibleName: toolTipText
        onClicked: jobPopup.open()
    }

    Popup {
        id: jobPopup
        parent: control
        x: control.width - width
        y: control.height + 4
        width: 360
        padding: 14
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        implicitHeight: content.implicitHeight + topPadding + bottomPadding
        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.menuSurface
            border.width: 1
            border.color: Theme.borderStrong
        }
        contentItem: ColumnLayout {
            id: content
            spacing: 9

            Label {
                text: qsTr("Group similar photos")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Suggested stacks use nearby image similarity. Review each group before deciding; ratings stay untouched.")
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
            }
            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("Group selected photos (%L1)").arg(
                    control.workspace.selectedPhotoCount)
                enabled: control.workspace.selectedPhotoCount >= 2
                    && !control.workspace.reviewClusteringController.busy
                    && !control.workspace.similarReviewController.busy
                onClicked: control.workspace.startReviewClustering("selection")
            }
            ShadowButton {
                Layout.fillWidth: true
                text: qsTr("Group loaded photos in this view (%L1)").arg(
                    control.workspace.controller.filteredItemCount)
                enabled: control.workspace.controller.filteredItemCount >= 2
                    && !control.workspace.reviewClusteringController.busy
                    && !control.workspace.similarReviewController.busy
                onClicked: control.workspace.startReviewClustering("view")
            }
            ShadowButton {
                Layout.fillWidth: true
                visible: control.workspace.currentLibraryAlbumIsManual
                text: qsTr("Group current Manual Album: %1").arg(
                    control.workspace.currentLibraryAlbumName)
                enabled: visible && !control.workspace.reviewClusteringController.busy
                    && !control.workspace.similarReviewController.busy
                onClicked: control.workspace.startReviewClustering("album")
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Each run checks at most 240 photos. Pause resumes in this session; changing a source preview is rechecked on use.")
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }
            MenuSeparator { Layout.fillWidth: true }
            Label {
                Layout.fillWidth: true
                text: control.workspace.reviewClusteringController.statusText
                wrapMode: Text.WordWrap
                color: Theme.textPrimary
                font.pixelSize: Theme.fontMeta
            }
            ProgressBar {
                Layout.fillWidth: true
                visible: control.workspace.reviewClusteringController.busy
                    && control.workspace.reviewClusteringController.total > 0
                from: 0
                to: Math.max(1, control.workspace.reviewClusteringController.total)
                value: control.workspace.reviewClusteringController.processed
            }
            RowLayout {
                Layout.fillWidth: true
                visible: control.workspace.reviewClusteringController.hasStatus
                ShadowButton {
                    visible: control.workspace.reviewClusteringController.busy
                    text: control.workspace.reviewClusteringController.canResume
                        ? qsTr("Resume") : qsTr("Pause")
                    enabled: control.workspace.reviewClusteringController.busy
                        && !control.workspace.reviewClusteringController.cancelling
                    onClicked: {
                        if (control.workspace.reviewClusteringController.canResume)
                            control.workspace.reviewClusteringController.resume()
                        else
                            control.workspace.reviewClusteringController.pause()
                    }
                }
                Item { Layout.fillWidth: true }
                ShadowButton {
                    text: control.workspace.reviewClusteringController.busy
                        ? qsTr("Cancel") : qsTr("Clear groups")
                    enabled: !control.workspace.reviewClusteringController.cancelling
                    onClicked: {
                        if (control.workspace.reviewClusteringController.busy)
                            control.workspace.reviewClusteringController.cancel()
                        else
                            control.workspace.reviewClusteringController.clear()
                    }
                }
            }
        }
    }
}
