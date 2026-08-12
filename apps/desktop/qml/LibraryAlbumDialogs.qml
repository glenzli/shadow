pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick

Item {
    id: root

    required property var controller
    required property bool hasActiveLibraryFilter
    required property var manualAlbums
    readonly property bool modalVisible: createDialog.opened
        || membershipDialog.opened || manageDialog.opened
        || deleteDialog.opened

    function openCreate() {
        createDialog.open();
    }

    function openMembership(targets) {
        membershipDialog.openTargets(targets);
    }

    function openManage(albumId, albumName, albumKind) {
        manageDialog.openAlbum(albumId, albumName, albumKind);
    }

    LibraryAlbumCreateDialog {
        id: createDialog
        hostWidth: root.width
        controller: root.controller
        hasActiveLibraryFilter: root.hasActiveLibraryFilter
    }

    LibraryAlbumMembershipDialog {
        id: membershipDialog
        hostWidth: root.width
        controller: root.controller
        manualAlbums: root.manualAlbums
    }

    LibraryAlbumManageDialog {
        id: manageDialog
        hostWidth: root.width
        controller: root.controller
        onDeleteRequested: (albumId, albumName) => {
            deleteDialog.openAlbum(albumId, albumName);
        }
    }

    LibraryAlbumDeleteDialog {
        id: deleteDialog
        hostWidth: root.width
        controller: root.controller
    }
}
