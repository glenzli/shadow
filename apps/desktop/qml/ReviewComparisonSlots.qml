import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Entry for the ordinary two-pane comparison workspace. The current photo is
// placed on the left and the next visible Library photo is used as a convenient
// default on the right; neither choice becomes a culling decision.
ColumnLayout {
    id: comparisonEntry

    required property var review

    readonly property bool available:
        !review.controller.comparisonBusy
        && !review.controller.decisionBusy
        && !review.controller.scanning
        && !review.controller.refreshing
        && !review.controller.busy
        && !review.controller.loadingMore
        && !review.culling.arenaActive
        && review.selectedPhotoId.length > 0
        && review.selectedRepresentationId.length > 0
        && review.selectedVisualSource.length > 0
        && !review.comparison.compareMode

    spacing: 6

    ShadowButton {
        Layout.fillWidth: true
        variant: ShadowButton.Tinted
        text: qsTr("Compare photos")
        toolTipText: qsTr("Open a two-photo comparison without changing either photo")
        enabled: comparisonEntry.available
        onClicked: comparisonEntry.review.comparison.startQuickComparison()
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("The adjacent photo is selected initially; both panes can then move independently.")
        color: comparisonEntry.review.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fontMeta
    }

    Label {
        Layout.fillWidth: true
        visible: comparisonEntry.review.comparison.localComparisonStatus.length > 0
        text: comparisonEntry.review.comparison.localComparisonStatus
        color: Theme.warningText
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fontMeta
    }
}
