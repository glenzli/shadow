pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the grid/single-photo presentation, incremental paging, comparison
// overlay, empty/busy states, and the toolbar that controls this surface.
Rectangle {
    id: gallery

    required property var workspace
    signal openLibraryManagementRequested()
    signal openFacetBrowserRequested()
    signal openMetadataRequested()
    signal sharedGradeRequested(var anchorItem)
    signal exportRequested(var targets)

    color: Theme.window

    function forceGalleryFocus() {
        if (gallery.workspace.galleryPresentation
                === ReviewWorkspace.SinglePhotoFilmstrip)
            singlePhotoPreview.forceGalleryFocus()
        else if (gallery.workspace.galleryPresentation === ReviewWorkspace.Map
                 && mapLoader.item)
            mapLoader.item.forceGalleryFocus()
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
        onOpenFacetBrowserRequested: gallery.openFacetBrowserRequested()
        onOpenMetadataRequested: gallery.openMetadataRequested()
        onSharedGradeRequested: anchorItem =>
            gallery.sharedGradeRequested(anchorItem)
        onExportRequested: targets => gallery.exportRequested(targets)
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
            && gallery.workspace.galleryPresentation === ReviewWorkspace.JustifiedGrid
        enabled: visible
        focus: visible
        spacing: gallery.workspace.justifiedReviewLayout.spacing
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
            required property var items
            required property int rowHeight

            width: justifiedGrid.width
            // `rowHeight` is the image height. Captions are outside the
            // image geometry so every visible image retains its ratio.
            height: rowHeight + 48

            Repeater {
                model: justifiedRow.items

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

    ReviewSinglePreview {
        id: singlePhotoPreview
        anchors.top: reviewToolBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: !gallery.workspace.comparison.compareMode
            && gallery.workspace.galleryPresentation
                === ReviewWorkspace.SinglePhotoFilmstrip
        review: gallery.workspace
        model: gallery.workspace.controller.model
    }

    Loader {
        id: mapLoader
        anchors.top: reviewToolBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        active: !gallery.workspace.comparison.compareMode
            && gallery.workspace.galleryPresentation === ReviewWorkspace.Map
        visible: active

        sourceComponent: LibraryMapView {
            workspace: gallery.workspace
        }
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
            && gallery.workspace.galleryPresentation === ReviewWorkspace.JustifiedGrid
            && gallery.workspace.selectedPhotoId.length > 0
    }

    ReviewComparisonView {
        review: gallery.workspace
    }

    Rectangle {
        anchors.fill: parent
        visible: gallery.workspace.controller.busy
            && gallery.workspace.galleryPresentation !== ReviewWorkspace.Map
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
