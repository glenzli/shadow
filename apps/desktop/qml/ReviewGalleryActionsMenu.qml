pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls

// Secondary gallery commands share the toolbar's selection and busy guards.
// The workspace remains the owner of each operation and its confirmation UI.
ShadowMenu {
    id: menu
    required property var workspace
    property bool thumbnailScaleVisible: false
    property bool thumbnailScaleAvailable: true
    signal openLibraryManagementRequested()
    signal openMetadataRequested()
    signal sharedGradeRequested()
    signal thumbnailScaleRequested()
    width: 380

    ShadowMenuItem {
        visible: menu.thumbnailScaleAvailable && !menu.thumbnailScaleVisible
        height: visible ? implicitHeight : 0
        text: qsTr("Thumbnail scale")
        onTriggered: menu.thumbnailScaleRequested()
    }

    ShadowMenuItem {
        objectName: "reviewAutoAdvanceAction"
        text: qsTr("Auto advance after rating or flagging")
        checkable: true
        checked: menu.workspace.autoAdvanceDecisions
        onTriggered: menu.workspace.autoAdvanceDecisions = !menu.workspace.autoAdvanceDecisions
    }
    MenuSeparator { }

    ShadowMenuItem {
        text: qsTr("Assign and filter Library keywords")
        onTriggered: menu.workspace.openKeywordPanel()
    }
    ShadowMenuItem {
        text: qsTr("Open photo metadata")
        enabled: menu.workspace.selectedPhotoId.length > 0
        onTriggered: menu.openMetadataRequested()
    }
    ShadowMenuItem {
        text: qsTr("Set location for selected photos")
        enabled: menu.workspace.selectedPhotoCount > 0
            && !menu.workspace.controller.libraryMetadataBusy
        onTriggered: menu.workspace.openLocationBatch()
    }
    ShadowMenuItem {
        text: qsTr("Apply a shared Grade Node to selection")
        enabled: menu.workspace.selectedPhotoCount > 0
            && !menu.workspace.selectionContainsRemote()
        onTriggered: menu.sharedGradeRequested()
    }
    MenuSeparator { }
    ShadowMenuItem {
        text: menu.workspace.manualLibraryAlbums.length > 0
            ? qsTr("Add selected photos to a Manual Album")
            : qsTr("Create a Manual Album first")
        enabled: menu.workspace.selectedPhotoCount > 0
            && !menu.workspace.selectionContainsRemote()
            && menu.workspace.manualLibraryAlbums.length > 0
            && !menu.workspace.controller.libraryAlbumsBusy
        onTriggered: menu.workspace.addTargetsToManualAlbum(menu.workspace.batchSelectionTargets())
    }
    ShadowMenuItem {
        visible: menu.workspace.currentLibraryAlbumIsManual
        height: visible ? implicitHeight : 0
        text: qsTr("Remove selected photos from this Manual Album")
        enabled: menu.workspace.selectedPhotoCount > 0
            && !menu.workspace.selectionContainsRemote()
            && !menu.workspace.controller.libraryAlbumsBusy
        onTriggered: menu.workspace.controller.removePhotosFromManualLibraryAlbum(
            String(menu.workspace.currentLibraryAlbum.id), menu.workspace.batchSelectionTargets())
    }
    MenuSeparator { }
    ShadowMenuItem {
        text: qsTr("Complete missing photo locations")
        enabled: !menu.workspace.controller.locationCompletionBusy
        onTriggered: menu.workspace.openLocationCompletion()
    }
    ShadowMenuItem {
        text: qsTr("Manage photo sources")
        onTriggered: menu.openLibraryManagementRequested()
    }
}
