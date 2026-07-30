pragma ComponentBehavior: Bound
pragma Translator: "LibraryKeywords"

import QtQuick
import QtQuick.Controls

Popup {
    id: root

    required property var workspace

    parent: Overlay.overlay
    modal: false
    focus: true
    width: Math.min(620, parent.width - 32)
    height: Math.min(610, parent.height - 64)
    x: Math.max(16, parent.width - width - 16)
    y: 48
    padding: 14
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function present() {
        root.workspace.controller.refreshLibraryKeywords()
        root.workspace.controller.requestLibraryKeywordsForPhoto(
            root.workspace.selectedPhotoId)
        open()
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: LibraryKeywordPanel {
        controller: root.workspace.controller
        targets: root.workspace.batchSelectionTargets()
        primaryPhotoId: root.workspace.selectedPhotoId
        allowAssignment: true
        allowFiltering: true
        manageTaxonomy: false
    }
}
