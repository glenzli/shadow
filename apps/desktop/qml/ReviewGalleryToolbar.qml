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
    signal openFacetBrowserRequested()
    signal openMetadataRequested()
    signal sharedGradeRequested(var anchorItem)
    signal exportRequested(var targets)
    anchors.top: parent.top
    anchors.left: parent.left
    anchors.right: parent.right
    height: 42
    z: 3
    visible: !toolbar.workspace.comparison.compareMode
    color: Theme.chrome

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
            checked: toolbar.workspace.hasLibraryFacetFilter
            source: "qrc:/icons/filter.svg"
            toolTipText: qsTr("Browse Library facets")
            accessibleName: toolTipText
            onClicked: toolbar.openFacetBrowserRequested()
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
            stepSize: 4
            value: toolbar.workspace.justifiedReviewLayout.targetRowHeight
            toolTipText: qsTr("Thumbnail scale")
            Accessible.name: toolTipText
            onMoved: {
                const next = Math.round(value)
                toolbar.workspace.justifiedReviewLayout.targetRowHeight = next
                toolbar.workspace.preferences.libraryThumbnailScale = next
            }
        }

        ShadowIconButton {
            visible: toolbar.workspace.galleryPresentation
                === ReviewWorkspace.JustifiedGrid
            source: "qrc:/icons/fit-view.svg"
            toolTipText: qsTr("Restore default thumbnail scale")
            accessibleName: toolTipText
            onClicked: {
                toolbar.workspace.justifiedReviewLayout.targetRowHeight = 188
                toolbar.workspace.preferences.libraryThumbnailScale = 188
            }
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
            source: "qrc:/icons/shared-link.svg"
            toolTipText: qsTr("Apply a shared Grade Node to selection")
            accessibleName: toolTipText
            enabled: toolbar.workspace.selectedPhotoCount > 0
            onClicked: toolbar.sharedGradeRequested(
                applySharedGradeButton)
        }

        ShadowIconButton {
            id: addToManualAlbumButton
            source: "qrc:/icons/add-folder.svg"
            toolTipText: qsTr("Add selected photos to a Manual Album")
            accessibleName: toolTipText
            enabled: toolbar.workspace.selectedPhotoCount > 0
                && toolbar.workspace.manualLibraryAlbums.length > 0
                && !toolbar.workspace.controller.libraryAlbumsBusy
            onClicked: toolbar.workspace.addTargetsToManualAlbum(
                toolbar.workspace.batchSelectionTargets())
        }

        ShadowIconButton {
            visible: toolbar.workspace.currentLibraryAlbumIsManual
            source: "qrc:/icons/clear.svg"
            toolTipText: qsTr("Remove selected photos from this Manual Album")
            accessibleName: toolTipText
            enabled: toolbar.workspace.selectedPhotoCount > 0
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
            onClicked: toolbar.exportRequested(
                toolbar.workspace.batchSelectionTargets())
        }

        ShadowIconButton {
            source: "qrc:/icons/edit.svg"
            variant: ShadowIconButton.Tinted
            toolTipText: qsTr("Open selected photo in Precision")
            accessibleName: toolTipText
            enabled: toolbar.workspace.canOpenSelectedPhoto
            onClicked: toolbar.workspace.openSelectedPhoto()
        }
    }
}
