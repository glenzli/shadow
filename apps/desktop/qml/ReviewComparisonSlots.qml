import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns comparison-slot assignment, admission, clearing, and entry.
ColumnLayout {
    id: slots

    required property var review

    readonly property bool mutationsAvailable:
        !slots.review.controller.comparisonBusy
        && !slots.review.controller.decisionBusy
        && !slots.review.controller.scanning
        && !slots.review.controller.refreshing
        && !slots.review.controller.busy
        && !slots.review.controller.loadingMore
    readonly property bool selectedVisualAvailable:
        slots.review.selectedPhotoId.length > 0
        && slots.review.selectedRepresentationId.length > 0
        && slots.review.selectedVisualHandle.length > 0
        && slots.review.selectedVisualSource.length > 0

    spacing: 8

    RowLayout {
        Layout.fillWidth: true

        Label {
            Layout.fillWidth: true
            text: qsTranslate("ReviewWorkspace", "COMPARE SLOTS")
            color: slots.review.textMuted
            font.pixelSize: 10
            font.weight: Font.DemiBold
        }

        ShadowIconButton {
            source: "qrc:/icons/compare.svg"
            iconSize: 17
            variant: ShadowIconButton.Tinted
            toolTipText: qsTranslate(
                "ReviewWorkspace", "Compare slots A and B")
            accessibleName: toolTipText
            enabled: slots.review.comparison.comparisonReady
                && !slots.review.comparison.compareMode
                && slots.mutationsAvailable
            onClicked: slots.review.comparison.enterComparison()
        }

        ShadowIconButton {
            source: "qrc:/icons/clear.svg"
            iconSize: 16
            toolTipText: qsTranslate(
                "ReviewWorkspace", "Clear comparison slots")
            accessibleName: toolTipText
            enabled: slots.mutationsAvailable
                && (slots.review.comparison.leftComparisonSnapshot !== null
                    || slots.review.comparison.rightComparisonSnapshot
                        !== null)
            onClicked:
                slots.review.comparison.clearComparisonSlots()
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 34
        radius: 6
        color: Theme.surfaceSubtle

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 3
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: slots.review.comparison.leftComparisonSnapshot
                    ? qsTranslate("ReviewWorkspace", "A  %1").arg(
                        slots.review.comparison.leftComparisonSnapshot.title)
                    : qsTranslate("ReviewWorkspace", "A  Not set")
                color: slots.review.comparison.leftComparisonSnapshot
                    ? slots.review.textPrimary : slots.review.textMuted
                elide: Text.ElideMiddle
                font.pixelSize: 10
                font.weight: Font.Medium
            }

            ShadowIconButton {
                buttonSize: 28
                source: "qrc:/icons/slot-left.svg"
                toolTipText: qsTranslate(
                    "ReviewWorkspace",
                    "Set selected photo as comparison slot A")
                accessibleName: toolTipText
                enabled: !slots.review.comparison.compareMode
                    && slots.mutationsAvailable
                    && slots.selectedVisualAvailable
                    && (slots.review.comparison.rightComparisonSnapshot
                            === null
                        || slots.review.comparison.rightComparisonSnapshot
                            .photoId !== slots.review.selectedPhotoId)
                onClicked:
                    slots.review.comparison.setSelectedAsLeft()
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 34
        radius: 6
        color: Theme.surfaceSubtle

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 3
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: slots.review.comparison.rightComparisonSnapshot
                    ? qsTranslate("ReviewWorkspace", "B  %1").arg(
                        slots.review.comparison.rightComparisonSnapshot.title)
                    : qsTranslate("ReviewWorkspace", "B  Not set")
                color: slots.review.comparison.rightComparisonSnapshot
                    ? slots.review.textPrimary : slots.review.textMuted
                elide: Text.ElideMiddle
                font.pixelSize: 10
                font.weight: Font.Medium
            }

            ShadowIconButton {
                buttonSize: 28
                source: "qrc:/icons/slot-right.svg"
                toolTipText: qsTranslate(
                    "ReviewWorkspace",
                    "Set selected photo as comparison slot B")
                accessibleName: toolTipText
                enabled: !slots.review.comparison.compareMode
                    && slots.mutationsAvailable
                    && slots.selectedVisualAvailable
                    && (slots.review.comparison.leftComparisonSnapshot
                            === null
                        || slots.review.comparison.leftComparisonSnapshot
                            .photoId !== slots.review.selectedPhotoId)
                onClicked:
                    slots.review.comparison.setSelectedAsRight()
            }
        }
    }

    Label {
        Layout.fillWidth: true
        visible: slots.review.selectedPhotoId.length > 0
            && slots.review.selectedVisualSource.length === 0
        text: qsTranslate(
            "ReviewWorkspace",
            "A display visual is required for comparison.")
        color: Theme.warningNoticeText
        wrapMode: Text.WordWrap
        font.pixelSize: 10
    }
}
