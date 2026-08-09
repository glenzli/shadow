pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Presents gallery scope, layout, and batch commands. Operations that open
// external surfaces are emitted as intent so this toolbar owns no popup state.
Rectangle {
    id: toolbar

    required property var workspace
    signal openLibraryManagementRequested()
    signal openMetadataRequested()
    signal sharedGradeRequested(var anchorItem)
    signal exportRequested(var targets)
    anchors.top: parent.top
    anchors.left: parent.left
    anchors.right: parent.right
    height: 42
    z: 3
    visible: !toolbar.workspace.comparison.compareMode
        && !toolbar.workspace.culling.arenaActive
    color: Theme.chrome

    function setGalleryScale(value) {
        const next = Math.round(value)
        toolbar.workspace.justifiedReviewLayout.targetRowHeight = next
        toolbar.workspace.preferences.libraryThumbnailScale = next
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: toolbar.workspace.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 12
        spacing: 8

        Label {
            text: toolbar.workspace.currentLibraryAlbumName.length > 0
                ? toolbar.workspace.currentLibraryAlbumName.toUpperCase()
                : qsTr("ALL PHOTOS")
            color: toolbar.workspace.textMuted
            font.pixelSize: 9
            font.weight: Font.DemiBold
            font.letterSpacing: 0.8
        }

        Label {
            text: qsTr("%L1 visible").arg(
                toolbar.workspace.controller.filteredItemCount)
            color: toolbar.workspace.textPrimary
            font.pixelSize: 11
        }

        ShadowIconButton {
            source: "qrc:/icons/library-manage.svg"
            toolTipText: qsTr("Manage photo sources")
            accessibleName: toolTipText
            onClicked: toolbar.openLibraryManagementRequested()
        }

        ShadowIconButton {
            checkable: true
            checked: toolbar.workspace.hasLibraryKeywordFilter
            source: "qrc:/icons/tag.svg"
            toolTipText: qsTr("Assign and filter Library keywords")
            accessibleName: toolTipText
            onClicked: toolbar.workspace.openKeywordPanel()
        }

        Item { Layout.fillWidth: true }

        Item {
            visible: toolbar.workspace.culling.candidateCount > 0
            implicitWidth: 32
            implicitHeight: 28

            ShadowIconButton {
                id: candidateArenaButton
                anchors.centerIn: parent
                source: "qrc:/icons/candidate.svg"
                variant: ShadowIconButton.Tinted
                enabled: toolbar.workspace.culling.canStartArena
                toolTipText: enabled
                    ? qsTr("Open the candidate duel")
                    : qsTr("Add at least two photos to start the candidate duel")
                accessibleName: toolTipText
                onClicked: toolbar.workspace.culling.startArena()
            }

            Rectangle {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: Math.max(13, candidateCountLabel.implicitWidth + 5)
                height: 13
                radius: height / 2
                color: candidateArenaButton.enabled ? Theme.accent : Theme.textDisabled

                Label {
                    id: candidateCountLabel
                    anchors.centerIn: parent
                    text: toolbar.workspace.culling.candidateCount
                    color: Theme.accentSelectionText
                    font.pixelSize: 8
                    font.weight: Font.Bold
                }
            }
        }

        ShadowIconButton {
            source: "qrc:/icons/review-grid.svg"
            selected: toolbar.workspace.galleryPresentation
                === ReviewWorkspace.JustifiedGrid
            toolTipText: qsTr("Browse as a photo grid")
            accessibleName: toolTipText
            onClicked: toolbar.workspace.galleryPresentation
                = ReviewWorkspace.JustifiedGrid
        }

        ShadowIconButton {
            source: "qrc:/icons/filmstrip.svg"
            selected: toolbar.workspace.galleryPresentation
                === ReviewWorkspace.SinglePhotoFilmstrip
            toolTipText: qsTr("Review one photo with a filmstrip")
            accessibleName: toolTipText
            onClicked: toolbar.workspace.galleryPresentation
                = ReviewWorkspace.SinglePhotoFilmstrip
        }

        ShadowIconButton {
            source: "qrc:/icons/map.svg"
            selected: toolbar.workspace.galleryPresentation
                === ReviewWorkspace.Map
            toolTipText: qsTr("Browse geotagged photos on a map")
            accessibleName: toolTipText
            onClicked: toolbar.workspace.galleryPresentation
                = ReviewWorkspace.Map
        }

        Label {
            visible: toolbar.workspace.galleryPresentation
                === ReviewWorkspace.JustifiedGrid
            text: qsTr("SCALE")
            color: toolbar.workspace.textMuted
            font.pixelSize: 9
            font.weight: Font.DemiBold
            font.letterSpacing: 0.7
        }

        ShadowInlineSlider {
            id: galleryScaleSlider
            visible: toolbar.workspace.galleryPresentation
                === ReviewWorkspace.JustifiedGrid
            Layout.preferredWidth: 138
            from: 96
            to: 360
            neutralValue: 188
            fillFromMinimum: true
            stepSize: 4
            value: toolbar.workspace.justifiedReviewLayout.targetRowHeight
            toolTipText: qsTr("Thumbnail scale")
            Accessible.name: toolTipText
            onMoved: toolbar.setGalleryScale(value)
            onResetRequested: value => toolbar.setGalleryScale(value)
        }

        ShadowIconButton {
            visible: toolbar.workspace.galleryPresentation
                === ReviewWorkspace.JustifiedGrid
            source: "qrc:/icons/fit-view.svg"
            toolTipText: qsTr("Restore default thumbnail scale")
            accessibleName: toolTipText
            onClicked: toolbar.setGalleryScale(188)
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 18
            color: toolbar.workspace.border
        }

        ShadowIconButton {
            source: "qrc:/icons/metadata.svg"
            toolTipText: qsTr("Open photo metadata")
            accessibleName: toolTipText
            enabled: toolbar.workspace.selectedPhotoId.length > 0
            onClicked: toolbar.openMetadataRequested()
        }

        Label {
            visible: toolbar.workspace.selectedPhotoCount > 1
            text: qsTr("%L1 selected").arg(
                toolbar.workspace.selectedPhotoCount)
            color: toolbar.workspace.textMuted
            font.pixelSize: 9
        }

        ShadowIconButton {
            id: applySharedGradeButton
            source: "qrc:/icons/shared-node.svg"
            toolTipText: qsTr("Apply a shared Grade Node to selection")
            accessibleName: toolTipText
            enabled: toolbar.workspace.selectedPhotoCount > 0
                && !toolbar.workspace.selectionContainsRemote()
            onClicked: toolbar.sharedGradeRequested(
                applySharedGradeButton)
        }

        ShadowIconButton {
            id: comparePhotosButton
            source: "qrc:/icons/compare.svg"
            toolTipText: toolbar.workspace.selectedPhotoCount === 2
                ? qsTr("Compare the two selected photos")
                : qsTr("Compare the selected photo with the next photo")
            accessibleName: toolTipText
            enabled: toolbar.workspace.selectedPhotoCount > 0
                && toolbar.workspace.selectedRepresentationId.length > 0
                && toolbar.workspace.selectedVisualSource.length > 0
                && !toolbar.workspace.controller.comparisonBusy
                && !toolbar.workspace.controller.decisionBusy
                && !toolbar.workspace.controller.scanning
                && !toolbar.workspace.controller.refreshing
                && !toolbar.workspace.controller.busy
                && !toolbar.workspace.controller.loadingMore
            onClicked: {
                if (toolbar.workspace.selectedPhotoCount === 2)
                    toolbar.workspace.compareSelectedPhotos()
                else
                    toolbar.workspace.comparison.startQuickComparison()
            }
        }

        ShadowIconButton {
            id: addToManualAlbumButton
            source: "qrc:/icons/album-add.svg"
            toolTipText: toolbar.workspace.manualLibraryAlbums.length > 0
                ? qsTr("Add selected photos to a Manual Album")
                : qsTr("Create a Manual Album first")
            accessibleName: toolTipText
            enabled: toolbar.workspace.selectedPhotoCount > 0
                && !toolbar.workspace.selectionContainsRemote()
                && toolbar.workspace.manualLibraryAlbums.length > 0
                && !toolbar.workspace.controller.libraryAlbumsBusy
            onClicked: toolbar.workspace.addTargetsToManualAlbum(
                toolbar.workspace.batchSelectionTargets())
        }

        ShadowIconButton {
            visible: toolbar.workspace.currentLibraryAlbumIsManual
            source: "qrc:/icons/album-remove.svg"
            toolTipText: qsTr("Remove selected photos from this Manual Album")
            accessibleName: toolTipText
            enabled: toolbar.workspace.selectedPhotoCount > 0
                && !toolbar.workspace.selectionContainsRemote()
                && !toolbar.workspace.controller.libraryAlbumsBusy
            onClicked: toolbar.workspace.controller.removePhotosFromManualLibraryAlbum(
                String(toolbar.workspace.currentLibraryAlbum.id),
                toolbar.workspace.batchSelectionTargets())
        }

        ShadowIconButton {
            source: "qrc:/icons/export.svg"
            toolTipText: qsTr("Export selected photos")
            accessibleName: toolTipText
            enabled: toolbar.workspace.selectedPhotoCount > 0
                && !toolbar.workspace.selectionContainsRemote()
            onClicked: toolbar.exportRequested(
                toolbar.workspace.batchSelectionTargets())
        }

    }
}
