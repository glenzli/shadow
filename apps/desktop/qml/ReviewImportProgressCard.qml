import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Presents one import/refresh progress receipt without owning import commands.
Rectangle {
    id: progressCard

    required property var workspace

    Layout.preferredHeight: progressColumn.implicitHeight + 20
    visible: progressCard.workspace.controller.scanning
        || (progressCard.workspace.controller.refreshing
            && Number(progressCard.workspace.controller.scanProgress.scanId)
                > 0)
    radius: 7
    color: Theme.panelInset
    border.color: Theme.borderStrong

    ColumnLayout {
        id: progressColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 5

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: progressCard.workspace.controller.refreshing
                        && !progressCard.workspace.controller.scanning
                    ? qsTranslate("ReviewWorkspace", "LIBRARY REFRESH")
                    : progressCard.workspace.controller.scanProgress.phase
                            === "cancelling"
                        ? qsTranslate("ReviewWorkspace", "STOPPING IMPORT")
                        : qsTranslate("ReviewWorkspace", "IMPORTING")
                color: progressCard.workspace.accent
                font.pixelSize: 8
                font.weight: Font.Bold
                font.letterSpacing: 0.9
            }

            Label {
                Layout.fillWidth: true
                text: qsTranslate(
                    "ReviewWorkspace", "%L1 catalogued").arg(
                        progressCard.workspace.controller.scanProgress
                            .cataloguedFiles)
                color: progressCard.workspace.textPrimary
                horizontalAlignment: Text.AlignRight
                font.pixelSize: 9
            }
        }

        Label {
            Layout.fillWidth: true
            text: progressCard.workspace.controller.scanning
                ? qsTranslate(
                    "ReviewWorkspace",
                    "%L1 supported · %L2 preview checks queued · %L3 filesystem issues")
                    .arg(progressCard.workspace.controller.scanProgress
                        .supportedFiles)
                    .arg(progressCard.workspace.controller.scanProgress
                        .decodeQueued)
                    .arg(progressCard.workspace.controller.scanProgress
                        .issueCount)
                : qsTranslate(
                    "ReviewWorkspace",
                    "%L1 supported · %L2/%L3 preview checks completed · %L4 filesystem issues")
                    .arg(progressCard.workspace.controller.scanProgress
                        .supportedFiles)
                    .arg(progressCard.workspace.controller.scanProgress
                        .decodeCompleted)
                    .arg(progressCard.workspace.controller.scanProgress
                        .decodeQueued)
                    .arg(progressCard.workspace.controller.scanProgress
                        .issueCount)
            color: progressCard.workspace.textMuted
            elide: Text.ElideRight
            font.pixelSize: 9
        }

        Label {
            Layout.fillWidth: true
            visible: !progressCard.workspace.controller.scanning
            text: qsTranslate(
                "ReviewWorkspace",
                "%L1 decode failures · %L2 preview failures · %L3 cancelled")
                .arg(progressCard.workspace.controller.scanProgress
                    .decodeHardFailures)
                .arg(progressCard.workspace.controller.scanProgress
                    .previewFailures)
                .arg(progressCard.workspace.controller.scanProgress
                    .decodeCancelled)
            color: progressCard.workspace.textMuted
            elide: Text.ElideRight
            font.pixelSize: 9
        }

        ProgressBar {
            Layout.fillWidth: true
            indeterminate: true
        }
    }
}
