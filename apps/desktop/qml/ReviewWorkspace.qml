pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: review

    required property var controller
    required property var justifiedReviewLayout
    required property var preferences

    ReviewSelectionState {
        id: selectionState
        controller: review.controller
        onPrimaryContextInvalidated: review.precisionOpenStatus = ""
    }

    ReviewComparisonState {
        id: comparisonState
        controller: review.controller
        selection: selectionState
        onComparisonRecorded: justifiedGrid.forceActiveFocus()
    }

    readonly property alias comparison: comparisonState
    readonly property alias selectedPhotoId: selectionState.selectedPhotoId
    readonly property alias selectedRepresentationId:
        selectionState.selectedRepresentationId
    readonly property alias selectedVisualHandle: selectionState.selectedVisualHandle
    readonly property alias selectedDecisionHeadSequence:
        selectionState.selectedDecisionHeadSequence
    readonly property alias selectedDecisionFlag: selectionState.selectedDecisionFlag
    readonly property alias selectedDecisionRating: selectionState.selectedDecisionRating
    readonly property alias selectedLiked: selectionState.selectedLiked
    readonly property alias selectedColorLabel: selectionState.selectedColorLabel
    readonly property alias selectedTitle: selectionState.selectedTitle
    readonly property alias selectedPath: selectionState.selectedPath
    readonly property alias selectedRole: selectionState.selectedRole
    readonly property alias selectedVisualSource: selectionState.selectedVisualSource
    readonly property alias selectedWidth: selectionState.selectedWidth
    readonly property alias selectedHeight: selectionState.selectedHeight
    readonly property alias selectedHasMetadata: selectionState.selectedHasMetadata
    readonly property alias selectedCameraMake: selectionState.selectedCameraMake
    readonly property alias selectedCameraModel: selectionState.selectedCameraModel
    readonly property alias selectedLensMake: selectionState.selectedLensMake
    readonly property alias selectedLensModel: selectionState.selectedLensModel
    readonly property alias selectedCapturedAtUnixSeconds:
        selectionState.selectedCapturedAtUnixSeconds
    readonly property alias selectedIsoSpeed: selectionState.selectedIsoSpeed
    readonly property alias selectedExposureTimeSeconds:
        selectionState.selectedExposureTimeSeconds
    readonly property alias selectedApertureFNumber:
        selectionState.selectedApertureFNumber
    readonly property alias selectedFocalLengthMm: selectionState.selectedFocalLengthMm
    readonly property alias selectedFocalLength35mm:
        selectionState.selectedFocalLength35mm
    readonly property alias selectedRawWidth: selectionState.selectedRawWidth
    readonly property alias selectedRawHeight: selectionState.selectedRawHeight
    readonly property alias selectedSensorBits: selectionState.selectedSensorBits
    readonly property alias selectedCfaPattern: selectionState.selectedCfaPattern
    readonly property alias selectedDngVersion: selectionState.selectedDngVersion
    readonly property alias selectedHasTechnicalObservation:
        selectionState.selectedHasTechnicalObservation
    readonly property alias selectedTechnicalInputWidth:
        selectionState.selectedTechnicalInputWidth
    readonly property alias selectedTechnicalInputHeight:
        selectionState.selectedTechnicalInputHeight
    readonly property alias selectedTechnicalPreprocessingVersion:
        selectionState.selectedTechnicalPreprocessingVersion
    readonly property alias selectedTechnicalImplementationVersion:
        selectionState.selectedTechnicalImplementationVersion
    readonly property alias selectedMeanLuma: selectionState.selectedMeanLuma
    readonly property alias selectedP01Luma: selectionState.selectedP01Luma
    readonly property alias selectedP50Luma: selectionState.selectedP50Luma
    readonly property alias selectedP99Luma: selectionState.selectedP99Luma
    readonly property alias selectedNearBlackFraction:
        selectionState.selectedNearBlackFraction
    readonly property alias selectedNearWhiteFraction:
        selectionState.selectedNearWhiteFraction
    readonly property alias selectedLaplacianVariance:
        selectionState.selectedLaplacianVariance
    readonly property alias selectedEdgeEnergy: selectionState.selectedEdgeEnergy
    readonly property alias selectedPhotoCount: selectionState.selectedPhotoCount
    // The grid stays the broad library browser. The single presentation is a
    // deliberately focused culling surface that still consumes the same
    // filtered catalogue model.
    enum GalleryPresentation {
        JustifiedGrid,
        SinglePhotoFilmstrip
    }
    property int galleryPresentation: ReviewWorkspace.JustifiedGrid
    property string precisionOpenStatus: ""

    readonly property bool canMutateDecision: selectedPhotoId.length > 0
        && !comparison.compareMode
        && !controller.scanning && !controller.refreshing
        && !controller.busy && !controller.loadingMore
        && !controller.comparisonBusy && !controller.decisionBusy
    readonly property bool canOpenSelectedPhoto: selectedPhotoId.length > 0
        && selectedRepresentationId.length > 0 && selectedPath.length > 0
        && !comparison.compareMode
        && !controller.refreshing
        && !controller.busy && !controller.loadingMore
        && !controller.comparisonBusy && !controller.decisionBusy
    readonly property var currentLibraryAlbum: {
        const albums = controller.libraryAlbums
        const selectedId = String(controller.libraryAlbumId)
        for (let index = 0; index < albums.length; ++index) {
            if (String(albums[index].id) === selectedId)
                return albums[index]
        }
        return null
    }
    readonly property string currentLibraryAlbumName:
        currentLibraryAlbum === null ? "" : String(currentLibraryAlbum.name)
    readonly property bool currentLibraryAlbumIsManual:
        currentLibraryAlbum !== null
            && String(currentLibraryAlbum.kind) === "manual"
    readonly property bool hasLibraryFacetFilter:
        controller.filterCaptureMonth.length > 0
        || controller.filterCameraKey.length > 0
        || controller.filterLensKey.length > 0
    readonly property bool hasActiveLibraryFilter:
        controller.filterFlag !== "all"
        || controller.filterMinimumRating > 0
        || controller.filterColorLabel !== "all"
        || controller.filterEditState !== "all"
        || controller.filterLiked !== "all"
        || hasLibraryFacetFilter
    readonly property var manualLibraryAlbums: {
        const albums = controller.libraryAlbums
        const manualAlbums = []
        for (let index = 0; index < albums.length; ++index) {
            if (String(albums[index].kind) === "manual")
                manualAlbums.push(albums[index])
        }
        return manualAlbums
    }
    signal openPrecisionRequested(string photoId, string representationId,
                                  string sourcePath, string photoTitle,
                                  string previewSource)
    signal openLibraryManagementRequested()
    signal exportRequested(var targets)

    ReviewMetadataPresentation {
        id: metadataPresentation
        workspace: review
    }

    MetadataWindow {
        id: metadataWindow
        transientParent: review.Window.window
        preferences: review.preferences
        photoTitle: review.selectedTitle
        sourcePath: review.selectedPath
        hasMetadata: review.selectedHasMetadata
        metadataPending: review.controller.photoInspectionBusy
        metadataFailed: review.controller.photoInspectionFailed
        fields: metadataPresentation.metadataFields()
        onRetryRequested: review.controller.retryPhotoInspection()
    }

    LibraryFacetPopup {
        id: libraryFacetPopup
        controller: review.controller
    }

    LibraryAlbumDialogs {
        id: albumDialogs
        anchors.fill: parent
        controller: review.controller
        hasActiveLibraryFilter: review.hasActiveLibraryFilter
        manualAlbums: review.manualLibraryAlbums
    }

    ReviewSharedGradePicker {
        id: sharedGradePicker
        workspace: review
    }

    readonly property color panel: Theme.panel
    readonly property color panelRaised: Theme.panelRaised
    readonly property color border: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textSecondary: Theme.textSecondary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent

    Component.onCompleted: {
        justifiedReviewLayout.targetRowHeight = preferences.libraryThumbnailScale
    }

    function selectionKey(photoId, representationId) {
        return selectionState.selectionKey(photoId, representationId)
    }

    function isPhotoSelected(photoId, representationId) {
        return selectionState.isPhotoSelected(photoId, representationId)
    }

    function batchSelectionTargets() {
        return selectionState.batchSelectionTargets()
    }

    // `revisionId` is UUIDv7-based, so descending lexical order gives a
    // stable "recently published or revised" quick list today. A later
    // catalog usage counter can refine this list by frequency without
    // changing the menu's top-ten + More contract.
    function sharedNodeQuickList() {
        const nodes = controller.sharedGradeNodes.slice()
        nodes.sort((left, right) => String(right.revisionId).localeCompare(
            String(left.revisionId)))
        return nodes.slice(0, 10)
    }

    function hasMoreSharedNodes() {
        return controller.sharedGradeNodes.length > 10
    }

    function openSharedNodePicker(x, y) {
        sharedGradePicker.presentAt(x, y)
    }

    function addTargetsToManualAlbum(targets) {
        albumDialogs.openMembership(targets)
    }

    function updatePrimaryPhoto(card) {
        selectionState.updatePrimaryPhoto(card)
    }

    function selectPhoto(card, modifiers) {
        selectionState.selectPhoto(card, modifiers)
    }

    function clearPrimaryPhoto() {
        selectionState.clearPrimaryPhoto()
    }

    function applySystemCollection(kind) {
        controller.clearFilters()
        if (kind === "liked")
            controller.filterLiked = "liked"
        else if (kind === "five-star")
            controller.filterMinimumRating = 5
    }

    function isSystemCollectionActive(kind) {
        if (controller.libraryAlbumId.length > 0)
            return false
        if (kind === "all")
            return !hasActiveLibraryFilter
        if (kind === "liked")
            return controller.filterLiked === "liked"
                && controller.filterMinimumRating === 0
                && controller.filterFlag === "all"
                && controller.filterColorLabel === "all"
                && controller.filterEditState === "all"
                && !hasLibraryFacetFilter
        if (kind === "five-star")
            return controller.filterMinimumRating === 5
                && controller.filterLiked === "all"
                && controller.filterFlag === "all"
                && controller.filterColorLabel === "all"
                && controller.filterEditState === "all"
                && !hasLibraryFacetFilter
        return false
    }

    function clearSelection() {
        selectionState.clearSelection()
    }

    function openSelectedPhoto() {
        if (!canOpenSelectedPhoto)
            return
        openPrecisionRequested(selectedPhotoId, selectedRepresentationId,
                               selectedPath, selectedTitle, selectedVisualSource)
    }

    function reportPrecisionOpenFailure(message) {
        precisionOpenStatus = String(message)
    }

    function setSelectedFlag(flag) {
        if (canMutateDecision)
            controller.setPhotoFlag(selectedPhotoId, flag)
    }

    function setSelectedRating(rating) {
        if (canMutateDecision)
            controller.setPhotoRating(selectedPhotoId, rating)
    }

    Connections {
        target: review.controller
        function onItemCountChanged() {
            if (review.controller.itemCount === 0)
                review.clearSelection()
        }
        function onDecisionChanged(photoId, headSequence, flag, rating) {
            selectionState.applyDecisionChanged(photoId, headSequence, flag, rating)
        }
        function onColorLabelChanged(photoId, colorLabel) {
            selectionState.applyColorLabelChanged(photoId, colorLabel)
        }
        function onLikedChanged(photoId, liked) {
            selectionState.applyLikedChanged(photoId, liked)
        }
    }

    Shortcut {
        sequence: "1"
        enabled: review.visible && review.comparison.canSubmitComparison
        onActivated: review.comparison.submitComparison(0)
    }

    Shortcut {
        sequence: "2"
        enabled: review.visible && review.comparison.canSubmitComparison
        onActivated: review.comparison.submitComparison(1)
    }

    Shortcut {
        sequence: "3"
        enabled: review.visible && review.comparison.canSubmitComparison
        onActivated: review.comparison.submitComparison(2)
    }

    Shortcut {
        sequence: "4"
        enabled: review.visible && review.comparison.canSubmitComparison
        onActivated: review.comparison.submitComparison(3)
    }

    Shortcut {
        sequence: "0"
        enabled: review.visible && review.comparison.canSubmitComparison
        onActivated: review.comparison.submitComparison(4)
    }

    Shortcut {
        sequence: "Escape"
        enabled: review.visible && review.comparison.compareMode
            && !review.controller.comparisonBusy
        onActivated: review.comparison.exitComparison()
    }

    Shortcut {
        sequence: "P"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedFlag("picked")
    }

    Shortcut {
        sequence: "U"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedFlag("unflagged")
    }

    Shortcut {
        sequence: "X"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedFlag("rejected")
    }

    Shortcut {
        sequence: "0"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(0)
    }

    Shortcut {
        sequence: "1"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(1)
    }

    Shortcut {
        sequence: "2"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(2)
    }

    Shortcut {
        sequence: "3"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(3)
    }

    Shortcut {
        sequence: "4"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(4)
    }

    Shortcut {
        sequence: "5"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(5)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ReviewLibrarySidebar {
            workspace: review
            albumDialogs: albumDialogs
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.window

            Rectangle {
                id: reviewToolBar
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 42
                z: 3
                visible: !review.comparison.compareMode
                color: Theme.chrome

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: review.border
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 12
                    spacing: 8

                    Label {
                        text: review.currentLibraryAlbumName.length > 0
                            ? review.currentLibraryAlbumName.toUpperCase()
                            : qsTr("ALL PHOTOS")
                        color: review.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                    }

                    Label {
                        text: qsTr("%L1 visible").arg(
                            review.controller.filteredItemCount)
                        color: review.textPrimary
                        font.pixelSize: 11
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/library-manage.svg"
                        toolTipText: qsTr("Manage photo sources")
                        accessibleName: toolTipText
                        onClicked: review.openLibraryManagementRequested()
                    }

                    ShadowIconButton {
                        checkable: true
                        checked: review.hasLibraryFacetFilter
                        source: "qrc:/icons/filter.svg"
                        toolTipText: qsTr("Browse Library facets")
                        accessibleName: toolTipText
                        onClicked: libraryFacetPopup.open()
                    }

                    Item { Layout.fillWidth: true }

                    ShadowIconButton {
                        source: "qrc:/icons/review-grid.svg"
                        selected: review.galleryPresentation
                            === ReviewWorkspace.JustifiedGrid
                        toolTipText: qsTr("Browse as a photo grid")
                        accessibleName: toolTipText
                        onClicked: review.galleryPresentation
                            = ReviewWorkspace.JustifiedGrid
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/filmstrip.svg"
                        selected: review.galleryPresentation
                            === ReviewWorkspace.SinglePhotoFilmstrip
                        toolTipText: qsTr("Review one photo with a filmstrip")
                        accessibleName: toolTipText
                        onClicked: review.galleryPresentation
                            = ReviewWorkspace.SinglePhotoFilmstrip
                    }

                    Label {
                        visible: review.galleryPresentation
                            === ReviewWorkspace.JustifiedGrid
                        text: qsTr("SCALE")
                        color: review.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.7
                    }

                    ShadowInlineSlider {
                        id: galleryScaleSlider
                        visible: review.galleryPresentation
                            === ReviewWorkspace.JustifiedGrid
                        Layout.preferredWidth: 138
                        from: 96
                        to: 360
                        stepSize: 4
                        value: review.justifiedReviewLayout.targetRowHeight
                        toolTipText: qsTr("Thumbnail scale")
                        Accessible.name: toolTipText
                        onMoved: {
                            const next = Math.round(value)
                            review.justifiedReviewLayout.targetRowHeight = next
                            review.preferences.libraryThumbnailScale = next
                        }
                    }

                    ShadowIconButton {
                        visible: review.galleryPresentation
                            === ReviewWorkspace.JustifiedGrid
                        source: "qrc:/icons/fit-view.svg"
                        toolTipText: qsTr("Restore default thumbnail scale")
                        accessibleName: toolTipText
                        onClicked: {
                            review.justifiedReviewLayout.targetRowHeight = 188
                            review.preferences.libraryThumbnailScale = 188
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 18
                        color: review.border
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/metadata.svg"
                        toolTipText: qsTr("Open photo metadata")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoId.length > 0
                        onClicked: metadataWindow.present()
                    }

                    Label {
                        visible: review.selectedPhotoCount > 1
                        text: qsTr("%L1 selected").arg(
                            review.selectedPhotoCount)
                        color: review.textMuted
                        font.pixelSize: 9
                    }

                    ShadowIconButton {
                        id: applySharedGradeButton
                        source: "qrc:/icons/shared-link.svg"
                        toolTipText: qsTr("Apply a shared Grade Node to selection")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoCount > 0
                        onClicked: {
                            sharedGradePicker.presentFrom(
                                applySharedGradeButton)
                        }
                    }

                    ShadowIconButton {
                        id: addToManualAlbumButton
                        source: "qrc:/icons/add-folder.svg"
                        toolTipText: qsTr("Add selected photos to a Manual Album")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoCount > 0
                            && review.manualLibraryAlbums.length > 0
                            && !review.controller.libraryAlbumsBusy
                        onClicked: review.addTargetsToManualAlbum(
                            review.batchSelectionTargets())
                    }

                    ShadowIconButton {
                        visible: review.currentLibraryAlbumIsManual
                        source: "qrc:/icons/clear.svg"
                        toolTipText: qsTr("Remove selected photos from this Manual Album")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoCount > 0
                            && !review.controller.libraryAlbumsBusy
                        onClicked: review.controller.removePhotosFromManualLibraryAlbum(
                            String(review.currentLibraryAlbum.id),
                            review.batchSelectionTargets())
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/export.svg"
                        toolTipText: qsTr("Export selected photos")
                        accessibleName: toolTipText
                        enabled: review.selectedPhotoCount > 0
                        onClicked: review.exportRequested(
                            review.batchSelectionTargets())
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/edit.svg"
                        variant: ShadowIconButton.Tinted
                        toolTipText: qsTr("Open selected photo in Precision")
                        accessibleName: toolTipText
                        enabled: review.canOpenSelectedPhoto
                        onClicked: review.openSelectedPhoto()
                    }
                }
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
                visible: !review.comparison.compareMode
                    && review.galleryPresentation === ReviewWorkspace.JustifiedGrid
                enabled: visible
                focus: visible
                spacing: review.justifiedReviewLayout.spacing
                cacheBuffer: 900
                model: review.justifiedReviewLayout

                function maybeLoadMore() {
                    if (review.controller.hasMore
                            && !review.controller.scanning
                            && !review.controller.refreshing
                            && !review.controller.busy
                            && !review.controller.loadingMore
                            && contentY + height >= contentHeight
                                - Math.max(400,
                                    review.justifiedReviewLayout.targetRowHeight * 2)) {
                        review.controller.loadMore()
                    }
                }

                onWidthChanged: review.justifiedReviewLayout.availableWidth
                    = Math.max(0, Math.floor(width))
                Component.onCompleted: review.justifiedReviewLayout.availableWidth
                    = Math.max(0, Math.floor(width))
                onContentYChanged: maybeLoadMore()
                onHeightChanged: Qt.callLater(maybeLoadMore)
                onCountChanged: Qt.callLater(maybeLoadMore)

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
                            workspace: review
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
                visible: !review.comparison.compareMode
                    && review.galleryPresentation
                        === ReviewWorkspace.SinglePhotoFilmstrip
                review: review
                model: review.controller.model
            }

            Label {
                anchors.centerIn: justifiedGrid
                width: Math.min(420, justifiedGrid.width - 60)
                visible: justifiedGrid.visible && justifiedGrid.count === 0
                    && !review.controller.busy
                text: review.controller.scanning
                    ? qsTr("Searching the folder for supported photos…\nNew RAW files will appear here as they are catalogued.")
                    : review.controller.scanProgress.phase === "failed"
                    ? qsTr("Import stopped, and no RAW files are currently visible.\nAlready catalogued files remain safely stored.")
                    : qsTr("Add a folder to the local Library.\nShadow will show embedded previews immediately, then replace them with locally generated proxies.")
                color: review.textMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                lineHeight: 1.4
                z: 2
            }

            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 18
                visible: justifiedGrid.visible && review.controller.loadingMore
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
                review: review
                floating: true
                includeColorLabels: true
                visible: !review.comparison.compareMode
                    && review.galleryPresentation === ReviewWorkspace.JustifiedGrid
                    && review.selectedPhotoId.length > 0
            }

            ReviewComparisonView {
                review: review
            }

            Rectangle {
                anchors.fill: parent
                visible: review.controller.busy
                color: Theme.busyOverlay

                Column {
                    anchors.centerIn: parent
                    spacing: 14
                    BusyIndicator {
                        anchors.horizontalCenter: parent.horizontalCenter
                        running: true
                    }
                    Label {
                        text: review.controller.scanning
                            ? qsTr("Finding the first photos")
                            : qsTr("Loading local Library")
                        color: review.textPrimary
                        font.pixelSize: 14
                    }
                }
            }
        }

        ReviewPhotoInspector {
            review: review
            onOpenMetadataRequested: metadataWindow.present()
        }
    }
}
