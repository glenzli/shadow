pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the grouped grid/single-photo presentation, incremental paging,
// comparison overlay, empty/busy states, and its controlling toolbar.
Rectangle {
    id: gallery

    required property var workspace
    property bool similarStackCollapsed: true
    property bool clusterHadResults: false
    signal openLibraryManagementRequested()
    signal openMetadataRequested()
    signal sharedGradeRequested(var anchorItem)
    signal exportRequested(var targets)

    color: Theme.window

    function forceGalleryFocus() {
        if (gallery.workspace.galleryPresentation
                === ReviewWorkspace.SinglePhotoFilmstrip)
            singlePhotoPreview.forceGalleryFocus()
        else
            justifiedGrid.forceActiveFocus()
    }

    function navigateGrid(horizontalDelta, verticalDelta, modifiers) {
        let currentPhotoId = gallery.workspace.selectedPhotoId
        let currentRepresentationId = gallery.workspace.selectedRepresentationId
        for (let attempt = 0; attempt < 64; ++attempt) {
            const target = gallery.workspace.justifiedReviewLayout.navigationTarget(
                currentPhotoId, currentRepresentationId,
                horizontalDelta, verticalDelta)
            if (!target || String(target.photoId || "").length === 0)
                return
            const hidden = gallery.similarStackCollapsed
                && String(target.sectionKey || "").indexOf("similar-review") >= 0
                && !Boolean(target.sectionLeadRow)
            if (hidden) {
                currentPhotoId = String(target.photoId)
                currentRepresentationId = String(target.representationId)
                continue
            }
            gallery.workspace.selectPhoto(target, Number(modifiers || 0))
            justifiedGrid.positionViewAtIndex(
                Number(target.layoutRow), ListView.Contain)
            return
        }
    }

    function setSimilarStackCollapsed(collapsed) {
        if (collapsed) {
            const current = gallery.workspace.justifiedReviewLayout.navigationTarget(
                gallery.workspace.selectedPhotoId,
                gallery.workspace.selectedRepresentationId, 0, 0)
            if (current && String(current.sectionKey || "")
                    .indexOf("similar-review") >= 0
                    && !Boolean(current.sectionLeadRow)) {
                let anchor = null
                if (String(current.sectionKey) === "similar-review") {
                    const matches = gallery.workspace.similarReviewController.matches
                    if (matches.length > 0)
                        anchor = matches[0]
                } else if (String(current.sectionKey).indexOf(
                        "similar-review-cluster-") === 0) {
                    const index = Number(String(current.sectionKey).replace(
                        "similar-review-cluster-", ""))
                    const groups = gallery.workspace.reviewClusteringController.groups
                    if (index >= 0 && index < groups.length) {
                        const keys = groups[index].representationKeys
                        if (keys.length > 0) {
                            const parts = String(keys[0]).split("\u001f")
                            if (parts.length === 2)
                                anchor = { photoId: parts[0], representationId: parts[1] }
                        }
                    }
                }
                if (anchor !== null) {
                    const lead = gallery.workspace.justifiedReviewLayout.navigationTarget(
                        String(anchor.photoId), String(anchor.representationId), 0, 0)
                    if (lead && String(lead.photoId) === String(anchor.photoId)
                            && Boolean(lead.sectionLeadRow))
                        gallery.workspace.selectPhoto(lead, Qt.NoModifier)
                }
            }
        }
        similarStackCollapsed = collapsed
    }

    Connections {
        target: gallery.workspace.similarReviewController
        function onResultsChanged() {
            if (!gallery.workspace.similarReviewController.hasResults) {
                gallery.similarStackCollapsed = true
            } else {
                Qt.callLater(() => justifiedGrid.positionViewAtBeginning())
            }
        }
    }

    Connections {
        target: gallery.workspace.reviewClusteringController
        function onStateChanged() {
            const ready = gallery.workspace.reviewClusteringController.hasResults
            if (ready && !gallery.clusterHadResults)
                Qt.callLater(() => justifiedGrid.positionViewAtBeginning())
            gallery.clusterHadResults = ready
        }
    }

    ReviewGalleryToolbar {
        id: reviewToolBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        workspace: gallery.workspace
        onOpenLibraryManagementRequested:
            gallery.openLibraryManagementRequested()
        onOpenMetadataRequested: gallery.openMetadataRequested()
        onSharedGradeRequested: anchorItem =>
            gallery.sharedGradeRequested(anchorItem)
        onExportRequested: targets => gallery.exportRequested(targets)
    }

    Rectangle {
        anchors.top: reviewToolBar.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.max(0, Math.min(parent.width - 36,
            statusLabel.implicitWidth + 28))
        height: 32
        radius: Theme.controlRadius
        color: Theme.menuSurface
        border.width: 1
        border.color: Theme.borderStrong
        z: 4
        visible: gallery.workspace.reviewClusteringController.hasStatus
            || gallery.workspace.similarReviewController.busy
            || gallery.workspace.similarReviewStatus.length > 0
            || gallery.workspace.similarReviewController.errorText.length > 0
            || gallery.workspace.similarReviewController.hasResults
                && gallery.workspace.similarReviewController.shownResultCount <= 1

        Label {
            id: statusLabel
            anchors.centerIn: parent
            width: parent.width - 20
            text: gallery.workspace.reviewClusteringController.hasStatus
                ? gallery.workspace.reviewClusteringController.statusText
                : gallery.workspace.similarReviewStatus.length > 0
                ? gallery.workspace.similarReviewStatus
                : gallery.workspace.similarReviewController.errorText.length > 0
                    ? gallery.workspace.similarReviewController.errorText
                    : gallery.workspace.similarReviewController.statusText
            color: gallery.workspace.textPrimary
            elide: Text.ElideRight
            font.pixelSize: Theme.fontMeta
        }
    }

    Binding {
        target: gallery.workspace.reviewGalleryGrouping
        property: "baseSections"
        value: {
            const similar = gallery.workspace.similarReviewController
            if (similar.hasResults && similar.rankedRepresentationKeys.length > 1) {
                return [{
                    key: "similar-review",
                    title: qsTr("Similar review candidates"),
                    subtitle: qsTr("Image similarity near the selected photo · review each candidate"),
                    representationKeys: similar.rankedRepresentationKeys
                }]
            }
            const clusters = gallery.workspace.reviewClusteringController
            if (clusters.hasResults) {
                const sections = []
                const groups = clusters.groups
                for (let index = 0; index < groups.length; ++index) {
                    sections.push({
                        key: "similar-review-cluster-" + index,
                        title: qsTr("Candidate stack %L1").arg(index + 1),
                        subtitle: qsTr("Suggested by reciprocal nearby similarity · compare to confirm"),
                        representationKeys: groups[index].representationKeys
                    })
                }
                return sections
            }
            const semantic = gallery.workspace.semanticSearchController
            if (!semantic.hasResults)
                return []
            const sections = []
            if (semantic.highRepresentationKeys.length > 0) {
                sections.push({
                    key: "semantic-high",
                    title: qsTr("Highly related"),
                    subtitle: qsTr("Closest matches for “%1”").arg(
                        semantic.activeQuery),
                    representationKeys: semantic.highRepresentationKeys
                })
            }
            if (semantic.possibleRepresentationKeys.length > 0) {
                sections.push({
                    key: "semantic-possible",
                    title: qsTr("Similarity order"),
                    subtitle: qsTr("Relative similarity for your query; review the photos to confirm"),
                    representationKeys: semantic.possibleRepresentationKeys
                })
            }
            return sections
        }
    }

    Binding {
        target: gallery.workspace.justifiedReviewLayout
        property: "sections"
        value: gallery.workspace.reviewGalleryGrouping.sections
    }

    ListView {
        id: justifiedGrid
        objectName: "reviewJustifiedGrid"
        anchors.top: reviewToolBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        anchors.topMargin: 14
        anchors.bottomMargin: 18
        clip: true
        visible: !gallery.workspace.comparison.compareMode
            && !gallery.workspace.culling.arenaActive
            && gallery.workspace.galleryPresentation === ReviewWorkspace.JustifiedGrid
        enabled: visible
        focus: visible
        spacing: gallery.workspace.justifiedReviewLayout.spacing
        // The decision toolbar floats over the gallery. Keep a scrollable
        // content safe area so the final caption can move fully above it.
        bottomMargin: galleryDecisionToolbar.visible
            ? galleryDecisionToolbar.height + 30 : 18
        cacheBuffer: 900
        model: gallery.workspace.justifiedReviewLayout

        function maybeLoadMore() {
            if (gallery.workspace.controller.hasMore
                    && !gallery.workspace.controller.scanning
                    && !gallery.workspace.controller.refreshing
                    && !gallery.workspace.controller.busy
                    && !gallery.workspace.controller.loadingMore
                    && contentY + height >= contentHeight
                        - Math.max(400,
                            gallery.workspace.justifiedReviewLayout.targetRowHeight * 2)) {
                gallery.workspace.controller.loadMore()
            }
        }

        onWidthChanged: gallery.workspace.justifiedReviewLayout.availableWidth
            = Math.max(0, Math.floor(width))
        Component.onCompleted: gallery.workspace.justifiedReviewLayout.availableWidth
            = Math.max(0, Math.floor(width))
        onContentYChanged: maybeLoadMore()
        onHeightChanged: Qt.callLater(maybeLoadMore)
        onCountChanged: Qt.callLater(maybeLoadMore)
        Keys.onPressed: event => {
            let horizontal = 0
            let vertical = 0
            if (event.key === Qt.Key_Left)
                horizontal = -1
            else if (event.key === Qt.Key_Right)
                horizontal = 1
            else if (event.key === Qt.Key_Up)
                vertical = -1
            else if (event.key === Qt.Key_Down)
                vertical = 1
            else
                return
            gallery.navigateGrid(horizontal, vertical, event.modifiers)
            event.accepted = true
        }

        delegate: Item {
            id: justifiedRow
            required property string rowKind
            required property var items
            required property int rowHeight
            required property string sectionKey
            required property string sectionTitle
            required property string sectionSubtitle
            required property int sectionItemCount
            required property int sectionOrdinal
            readonly property bool similarGroupRow:
                sectionKey.indexOf("similar-review") >= 0
            readonly property bool collapsedSimilarPhotoRow:
                rowKind === "photos" && similarGroupRow
                && !Boolean(items.length > 0 && items[0].sectionLeadRow)
                && gallery.similarStackCollapsed

            width: justifiedGrid.width
            // `rowHeight` is the image height. Captions are outside the
            // image geometry so every visible image retains its ratio.
            height: rowKind === "section"
                ? (sectionOrdinal === 0 ? 38 : 66)
                : collapsedSimilarPhotoRow ? 0 : rowHeight + 48

            Rectangle {
                visible: justifiedRow.rowKind === "section"
                    && justifiedRow.sectionOrdinal > 0
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 14
                height: 1
                color: Theme.border
            }

            RowLayout {
                visible: justifiedRow.rowKind === "section"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 4
                spacing: 8

                Label {
                    text: justifiedRow.sectionTitle
                    color: gallery.workspace.textPrimary
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }

                Label {
                    text: qsTr("%L1 photos").arg(
                        justifiedRow.sectionItemCount)
                    color: gallery.workspace.textMuted
                    font.pixelSize: Theme.fontMeta
                }

                Label {
                    Layout.fillWidth: true
                    text: justifiedRow.sectionSubtitle
                    color: gallery.workspace.textMuted
                    font.pixelSize: Theme.fontMeta
                    elide: Text.ElideRight
                }

                ShadowButton {
                    visible: justifiedRow.similarGroupRow
                    compact: true
                    text: qsTr("Compare group")
                    onClicked: {
                        if (justifiedRow.sectionKey === "similar-review")
                            gallery.workspace.openSimilarComparison()
                        else
                            gallery.workspace.openReviewClusterComparison(
                                justifiedRow.sectionKey)
                    }
                }

                ShadowButton {
                    visible: justifiedRow.similarGroupRow
                    compact: true
                    text: gallery.similarStackCollapsed
                        ? qsTr("Show photos") : qsTr("Collapse stack")
                    onClicked: gallery.setSimilarStackCollapsed(
                        !gallery.similarStackCollapsed)
                }
            }

            Repeater {
                model: justifiedRow.rowKind === "photos"
                    && !justifiedRow.collapsedSimilarPhotoRow
                    ? justifiedRow.items : []

                delegate: ReviewPhotoCard {
                    required property var modelData
                    x: Number(modelData.layoutX)
                    y: 0
                    width: Number(modelData.layoutWidth)
                    height: justifiedRow.height
                    workspace: gallery.workspace
                    entry: modelData
                }
            }
        }
    }

    ReviewGallerySectionNavigator {
        anchors.right: justifiedGrid.right
        anchors.rightMargin: 8
        anchors.verticalCenter: justifiedGrid.verticalCenter
        z: 3
        view: justifiedGrid
        sectionAnchors: gallery.workspace.justifiedReviewLayout.sectionAnchors
        groupingActive: gallery.workspace.reviewGalleryGrouping.activeDimensionCount > 0
    }

    ReviewSinglePreview {
        id: singlePhotoPreview
        anchors.top: reviewToolBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: !gallery.workspace.comparison.compareMode
            && !gallery.workspace.culling.arenaActive
            && gallery.workspace.galleryPresentation
                === ReviewWorkspace.SinglePhotoFilmstrip
        review: gallery.workspace
        model: gallery.workspace.controller.model
    }

    Label {
        id: emptyState
        objectName: "reviewEmptyStateText"
        anchors.centerIn: justifiedGrid
        width: Math.min(420, justifiedGrid.width - 60)
        visible: justifiedGrid.visible && justifiedGrid.count === 0
            && !gallery.workspace.controller.busy
        text: gallery.workspace.controller.scanning
            ? qsTranslate("ReviewWorkspace", "Searching the folder for supported photos…\nNew RAW files will appear here as they are catalogued.")
            : gallery.workspace.controller.scanProgress.phase === "failed"
            ? qsTranslate("ReviewWorkspace", "Import stopped, and no RAW files are currently visible.\nAlready catalogued files remain safely stored.")
            : gallery.workspace.hasActiveLibraryFilter
            ? qsTranslate("ReviewWorkspace", "No photos match the current filters.")
            : qsTranslate("ReviewWorkspace", "Add a folder to the local Library.\nShadow will show embedded previews immediately, then replace them with locally generated proxies.")
        color: gallery.workspace.textMuted
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        lineHeight: 1.4
        z: 2
    }

    ShadowButton {
        objectName: "clearEmptyLibraryFiltersButton"
        anchors.top: emptyState.bottom
        anchors.topMargin: 12
        anchors.horizontalCenter: emptyState.horizontalCenter
        visible: emptyState.visible && gallery.workspace.hasActiveLibraryFilter
        text: qsTranslate("Main", "Clear all Library filters")
        onClicked: gallery.workspace.controller.clearFilters()
        z: 2
    }

    BusyIndicator {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 18
        visible: justifiedGrid.visible && gallery.workspace.controller.loadingMore
        running: visible
        width: 34
        height: 34
        z: 2
    }

    ReviewDecisionToolbar {
        id: galleryDecisionToolbar
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 18
        anchors.bottomMargin: 18
        z: 4
        review: gallery.workspace
        floating: true
        includeColorLabels: true
        visible: !gallery.workspace.comparison.compareMode
            && !gallery.workspace.culling.arenaActive
            && gallery.workspace.galleryPresentation === ReviewWorkspace.JustifiedGrid
            && gallery.workspace.selectedPhotoId.length > 0
    }

    ReviewComparisonView {
        review: gallery.workspace
    }

    ReviewCullingArena {
        anchors.fill: parent
        review: gallery.workspace
    }

    Rectangle {
        anchors.fill: parent
        visible: gallery.workspace.controller.busy
        color: Theme.busyOverlay

        Column {
            anchors.centerIn: parent
            spacing: 14
            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                running: true
            }
            Label {
                text: gallery.workspace.controller.scanning
                    ? qsTr("Finding the first photos")
                    : qsTr("Loading local Library")
                color: gallery.workspace.textPrimary
                font.pixelSize: Theme.fontSubheading
            }
        }
    }
}
