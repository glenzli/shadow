import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the session comparison-evidence summary and its bounded undo action.
ColumnLayout {
    id: evidence

    required property var workspace

    readonly property bool hasEvidence:
        evidence.workspace.controller.sessionEvidenceCount > 0
        || evidence.workspace.controller.canUndoComparison
    readonly property bool hasStatus:
        !evidence.workspace.comparison.compareMode
        && (evidence.workspace.controller.comparisonStatusText.length > 0
            || evidence.workspace.comparison.localComparisonStatus.length > 0)

    visible: hasEvidence || hasStatus
    spacing: 8

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        Layout.topMargin: 10
        visible: evidence.hasEvidence
        color: evidence.workspace.border
    }

    RowLayout {
        Layout.fillWidth: true
        visible: evidence.hasEvidence
        spacing: 6

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                text: qsTranslate("ReviewWorkspace", "COMPARE EVIDENCE")
                color: evidence.workspace.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTranslate(
                    "ReviewWorkspace", "%L1 active this session").arg(
                        evidence.workspace.controller.sessionEvidenceCount)
                color: evidence.workspace.textPrimary
                font.pixelSize: Theme.fontSection
            }
        }

        ShadowIconButton {
            source: "qrc:/icons/undo.svg"
            toolTipText: qsTranslate(
                "ReviewWorkspace", "Forget last comparison")
            accessibleName: toolTipText
            visible: enabled
            enabled: evidence.workspace.controller.canUndoComparison
                && !evidence.workspace.controller.comparisonBusy
                && !evidence.workspace.controller.decisionBusy
                && !evidence.workspace.controller.scanning
                && !evidence.workspace.controller.refreshing
                && !evidence.workspace.controller.busy
                && !evidence.workspace.controller.loadingMore
            onClicked:
                evidence.workspace.controller.undoLastComparison()
        }
    }

    Label {
        Layout.fillWidth: true
        visible: evidence.hasStatus
        text:
            evidence.workspace.comparison.localComparisonStatus.length > 0
            ? evidence.workspace.comparison.localComparisonStatus
            : evidence.workspace.controller.comparisonStatusText
        color: evidence.workspace.controller.comparisonBusy
            ? evidence.workspace.accent : evidence.workspace.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fontCaption
    }
}
