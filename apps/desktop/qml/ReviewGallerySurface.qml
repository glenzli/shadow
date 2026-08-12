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

    function navigateGrid(horizontalDelta, verticalDelta) {
        const target = gallery.workspace.justifiedReviewLayout.navigationTarget(
            gallery.workspace.selectedPhotoId,
            gallery.workspace.selectedRepresentationId,
            horizontalDelta,
            verticalDelta)
        if (!target || String(target.photoId || "").length === 0)
            return
        gallery.workspace.selectPhoto(target, 0)
        justifiedGrid.positionViewAtIndex(
            Number(target.layoutRow), ListView.Contain)
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

    Binding {
        target: gallery.workspace.reviewGalleryGrouping
        property: "baseSections"
        value: {
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
                    title: qsTr("Possibly related"),
                    subtitle: qsTr("Broader matches worth reviewing"),
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
            gallery.navigateGrid(horizontal, vertical)
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

            width: justifiedGrid.width
            // `rowHeight` is the image height. Captions are outside the
            // image geometry so every visible image retains its ratio.
            height: rowKind === "section"
                ? (sectionOrdinal === 0 ? 38 : 66)
                : rowHeight + 48

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
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }

                Label {
                    text: qsTr("%L1 photos").arg(
                        justifiedRow.sectionItemCount)
                    color: gallery.workspace.textMuted
                    font.pixelSize: 10
                }

                Label {
                    Layout.fillWidth: true
                    text: justifiedRow.sectionSubtitle
                    color: gallery.workspace.textMuted
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
            }

            Repeater {
                model: justifiedRow.rowKind === "photos"
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
        objectName: "reviewEmptyStateText"
        anchors.centerIn: justifiedGrid
        width: Math.min(420, justifiedGrid.width - 60)
        visible: justifiedGrid.visible && justifiedGrid.count === 0
            && !gallery.workspace.controller.busy
        text: gallery.workspace.controller.scanning
            ? qsTranslate("ReviewWorkspace", "Searching the folder for supported photos…\nNew RAW files will appear here as they are catalogued.")
            : gallery.workspace.controller.scanProgress.phase === "failed"
            ? qsTranslate("ReviewWorkspace", "Import stopped, and no RAW files are currently visible.\nAlready catalogued files remain safely stored.")
            : qsTranslate("ReviewWorkspace", "Add a folder to the local Library.\nShadow will show embedded previews immediately, then replace them with locally generated proxies.")
        color: gallery.workspace.textMuted
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        lineHeight: 1.4
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
                font.pixelSize: 14
            }
        }
    }
}
