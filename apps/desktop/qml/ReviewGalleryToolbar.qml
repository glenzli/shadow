pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Presents gallery scope, responsive semantic search, layout, and batch
// commands. Operations outside those toolbar-owned interactions are emitted
// as intent.
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
    height: 44
    z: 3
    visible: !toolbar.workspace.comparison.compareMode
        && !toolbar.workspace.culling.arenaActive
    color: Theme.chrome
    onVisibleChanged: {
        if (!visible) {
            actionsMenu.close()
            thumbnailScalePopup.close()
        }
    }

    readonly property bool showThumbnailScale: width >= 1050
        && workspace.galleryPresentation === ReviewWorkspace.JustifiedGrid

    ReviewGalleryActionsMenu {
        id: actionsMenu
        workspace: toolbar.workspace
        thumbnailScaleVisible: toolbar.showThumbnailScale
        onOpenLibraryManagementRequested: toolbar.openLibraryManagementRequested()
        onOpenMetadataRequested: toolbar.openMetadataRequested()
        onSharedGradeRequested: toolbar.sharedGradeRequested(moreButton)
        onThumbnailScaleRequested: thumbnailScalePopup.open()
    }

    Popup {
        id: thumbnailScalePopup
        parent: moreButton
        x: moreButton.width - width
        y: moreButton.height + 4
        width: 268
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 12
        implicitHeight: scalePopupContent.implicitHeight + topPadding + bottomPadding
        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.menuSurface
            border.width: 1
            border.color: Theme.borderStrong
        }
        contentItem: ColumnLayout {
            id: scalePopupContent
            spacing: 8
            Label {
                text: qsTr("Thumbnail scale")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
            }
            RowLayout {
                ShadowInlineSlider {
                    Layout.fillWidth: true
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
                    source: "qrc:/icons/fit-view.svg"
                    toolTipText: qsTr("Restore default thumbnail scale")
                    accessibleName: toolTipText
                    onClicked: toolbar.setGalleryScale(188)
                }
            }
        }
    }

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
            visible: toolbar.width >= 1100
            text: toolbar.workspace.currentLibraryScopeName
            color: toolbar.workspace.textMuted
            font.pixelSize: Theme.fontCaption
            font.weight: Font.DemiBold
            font.letterSpacing: 0.8
        }

        Label {
            text: qsTr("%L1 visible").arg(
                toolbar.workspace.controller.filteredItemCount)
            color: toolbar.workspace.textPrimary
            font.pixelSize: Theme.fontSection
        }

        SemanticSearchControl {
            id: semanticSearch
            Layout.preferredWidth: expanded ? Math.min(268, toolbar.width * 0.30) : implicitWidth
            Layout.preferredHeight: implicitHeight
            expanded: toolbar.width >= 650
            workspace: toolbar.workspace
        }

        ReviewGalleryGroupingControl {
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            grouping: toolbar.workspace.reviewGalleryGrouping
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
                    font.pixelSize: Theme.fontMicro
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

        Label {
            visible: toolbar.showThumbnailScale
            text: qsTr("SCALE")
            color: toolbar.workspace.textMuted
            font.pixelSize: Theme.fontCaption
            font.weight: Font.DemiBold
            font.letterSpacing: 0.7
        }

        ShadowInlineSlider {
            id: galleryScaleSlider
            visible: toolbar.showThumbnailScale
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
            visible: toolbar.showThumbnailScale
            source: "qrc:/icons/fit-view.svg"
            toolTipText: qsTr("Restore default thumbnail scale")
            accessibleName: toolTipText
            onClicked: toolbar.setGalleryScale(188)
        }

        Rectangle {
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            Layout.preferredWidth: 1
            Layout.preferredHeight: 18
            color: toolbar.workspace.border
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

        ShadowButton {
            id: mergeButton
            visible: toolbar.workspace.selectedPhotoCount >= 2
            objectName: "photoCompositionMenuButton"
            text: qsTr("Merge")
            compact: true
            variant: ShadowButton.Ghost
            toolTipText: qsTr("Merge 2–12 selected photos")
            enabled: toolbar.workspace.selectedPhotoCount >= 2
                && toolbar.workspace.selectedPhotoCount <= 12
                && !toolbar.workspace.controller.remoteLibraryBusy
            onClicked: mergeMenu.popup(mergeButton, 0, mergeButton.height)
            ShadowMenu {
                id: mergeMenu
                ShadowMenuItem { text: qsTr("HDR merge…"); onTriggered: toolbar.workspace.requestComposition("hdr") }
                ShadowMenuItem { text: qsTr("Panorama merge…"); onTriggered: toolbar.workspace.requestComposition("panorama") }
            }
        }

        ShadowIconButton {
            objectName: "reviewEditButton"
            source: "qrc:/icons/edit.svg"
            accessibleName: qsTr("Edit")
            variant: ShadowIconButton.Tinted
            enabled: toolbar.workspace.canOpenSelectedPhoto
            toolTipText: qsTr("Edit selected photo")
            onClicked: toolbar.workspace.openSelectedPhoto()
        }

        ShadowIconButton {
            objectName: "reviewExportButton"
            source: "qrc:/icons/export.svg"
            accessibleName: qsTr("Export…")
            enabled: toolbar.workspace.selectedPhotoCount > 0
                && !toolbar.workspace.controller.remoteLibraryBusy
            toolTipText: qsTr("Export selected photos")
            onClicked: toolbar.exportRequested(toolbar.workspace.batchSelectionTargets())
        }

        ShadowIconButton {
            id: moreButton
            objectName: "reviewMoreButton"
            source: "qrc:/icons/more-horizontal.svg"
            accessibleName: qsTr("More…")
            toolTipText: accessibleName
            selected: actionsMenu.opened
            onClicked: actionsMenu.popup(moreButton, moreButton.width - actionsMenu.width, moreButton.height)
        }
    }
}
