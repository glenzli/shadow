pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A Shadow-owned context menu for library photos. Native menus are visually
// inconsistent between macOS and Windows and cannot express the Library's
// compact, expandable collections without falling back to a separate dialog.
Popup {
    id: root

    // The Popup is visually reparented into Overlay.overlay. Keep only a
    // stable workspace reference and scalar photo values while it is open;
    // retaining bindings through a virtualized card would outlive that
    // delegate when the Review grid recycles its row.
    property var workspace: null
    property string photoId: ""
    property string locationId: ""
    property string photoTitle: ""
    property string photoSourcePath: ""
    property bool photoSourceAvailable: true
    property bool photoLiked: false
    property string photoDecisionFlag: "unflagged"
    property bool photoIsRemote: false
    property var photoSnapshot: null
    property bool albumsExpanded: false
    property bool ratingsExpanded: false
    property bool nodesExpanded: false
    readonly property bool hasWorkspace:
        workspace !== null && workspace !== undefined

    width: 258
    padding: 6
    modal: false
    focus: true
    parent: Overlay.overlay
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function openAt(item, localX, localY, workspaceValue, photoIdValue,
                    locationIdValue, titleValue, sourcePathValue,
                    sourceAvailableValue, likedValue, decisionFlagValue,
                    isRemoteValue, snapshotValue) {
        workspace = workspaceValue
        photoId = String(photoIdValue)
        locationId = String(locationIdValue)
        photoTitle = String(titleValue)
        photoSourcePath = String(sourcePathValue)
        photoSourceAvailable = Boolean(sourceAvailableValue)
        photoLiked = Boolean(likedValue)
        photoDecisionFlag = String(decisionFlagValue)
        photoIsRemote = Boolean(isRemoteValue)
        photoSnapshot = snapshotValue || null
        albumsExpanded = false
        nodesExpanded = false
        parent = Overlay.overlay
        const point = item.mapToItem(Overlay.overlay, localX, localY)
        x = Math.max(8, Math.min(point.x, workspace.width - width - 8))
        y = Math.max(8, Math.min(point.y, workspace.height - implicitHeight - 8))
        open()
    }

    function releaseOwner(ownerItem) {
        close()
        if (ownerItem)
            parent = ownerItem
        workspace = null
        photoId = ""
        locationId = ""
        photoTitle = ""
        photoSourcePath = ""
        photoSourceAvailable = true
        photoLiked = false
        photoDecisionFlag = "unflagged"
        photoIsRemote = false
        photoSnapshot = null
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    component Divider: Rectangle {
        width: parent ? parent.width : 0
        height: 1
        color: Theme.border
    }

    component MenuRow: Item {
        id: row
        required property string text
        property url iconSource: ""
        property bool actionEnabled: true
        property bool expandable: false
        property bool expanded: false
        property int indent: 0
        property color iconColor: Theme.textSecondary
        signal activated()

        width: parent ? parent.width : 0
        height: 34
        enabled: actionEnabled

        Rectangle {
            anchors.fill: parent
            radius: Theme.compactControlRadius
            color: rowMouse.containsMouse && row.actionEnabled
                ? Theme.buttonGhostHover : Theme.transparent
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 9 + row.indent
            anchors.rightMargin: 9
            spacing: 8

            ShadowIcon {
                visible: row.iconSource.toString().length > 0
                source: row.iconSource
                color: row.actionEnabled ? row.iconColor : Theme.textDisabled
                size: 15
            }

            Label {
                Layout.fillWidth: true
                text: row.text
                color: row.actionEnabled ? Theme.textPrimary : Theme.textDisabled
                font.pixelSize: Theme.fontSection
                elide: Text.ElideRight
            }

            ShadowIcon {
                visible: row.expandable
                source: "qrc:/icons/chevron-down.svg"
                color: row.actionEnabled ? Theme.textMuted : Theme.textDisabled
                size: 13
                rotation: row.expanded ? 180 : 0
            }
        }

        MouseArea {
            id: rowMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: row.actionEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            enabled: row.actionEnabled
            onClicked: row.activated()
        }
    }

    contentItem: Column {
        width: root.width - root.leftPadding - root.rightPadding
        spacing: 2

        MenuRow {
            text: qsTr("Open in Precision")
            iconSource: "qrc:/icons/edit.svg"
            actionEnabled: root.hasWorkspace
                && root.workspace.canOpenSelectedPhoto
            onActivated: {
                if (!root.hasWorkspace)
                    return
                root.workspace.openSelectedPhoto()
                root.close()
            }
        }

        MenuRow {
            text: root.hasWorkspace
                && root.workspace.culling.containsCandidate(root.photoSnapshot)
                ? qsTr("Remove from Candidates")
                : qsTr("Add to Candidates")
            iconSource: "qrc:/icons/compare.svg"
            actionEnabled: root.hasWorkspace
                && root.photoSnapshot !== null
                && String(root.photoSnapshot.visualSource || "").length > 0
                && !root.workspace.culling.arenaActive
            onActivated: {
                if (!root.hasWorkspace)
                    return
                root.workspace.culling.toggleCandidate(root.photoSnapshot)
                root.close()
            }
        }

        Divider {}

        MenuRow {
            visible: !root.photoSourceAvailable && !root.photoIsRemote
            text: qsTr("Add or locate folder…")
            iconSource: "qrc:/icons/add-folder.svg"
            iconColor: Theme.warningText
            actionEnabled: root.hasWorkspace && root.photoId.length > 0
            onActivated: {
                if (!root.hasWorkspace)
                    return
                root.workspace.relinkUnavailablePhoto(
                    root.photoId, root.locationId,
                    root.photoTitle, root.photoSourcePath)
                root.close()
            }
        }

        MenuRow {
            visible: !root.photoSourceAvailable && !root.photoIsRemote
            text: qsTr("Remove from Library…")
            iconSource: "qrc:/icons/trash.svg"
            iconColor: Theme.dangerText
            actionEnabled: root.hasWorkspace && root.photoId.length > 0
            onActivated: {
                if (!root.hasWorkspace)
                    return
                root.workspace.removeUnavailablePhoto(
                    root.photoId, root.photoTitle, root.photoSourcePath)
                root.close()
            }
        }

        Divider {
            visible: !root.photoSourceAvailable && !root.photoIsRemote
        }

        MenuRow {
            text: root.photoLiked ? qsTr("Remove Like") : qsTr("Like")
            iconSource: root.photoLiked
                ? "qrc:/icons/heart-filled.svg" : "qrc:/icons/heart.svg"
            iconColor: Theme.likeAccent
            actionEnabled: root.hasWorkspace
                && root.workspace.canMutateDecision
            onActivated: {
                if (!root.hasWorkspace)
                    return
                root.workspace.controller.setPhotoLiked(
                    root.photoId, !root.photoLiked)
                root.close()
            }
        }

        MenuRow {
            text: root.photoDecisionFlag === "picked"
                ? qsTr("Clear pick flag") : qsTr("Pick")
            iconSource: "qrc:/icons/pick.svg"
            actionEnabled: root.hasWorkspace
                && root.workspace.canMutateDecision
            onActivated: {
                if (!root.hasWorkspace)
                    return
                root.workspace.setSelectedFlag(
                    root.photoDecisionFlag === "picked" ? "unflagged" : "picked")
                root.close()
            }
        }

        MenuRow {
            text: root.photoDecisionFlag === "rejected"
                ? qsTr("Clear reject flag") : qsTr("Reject")
            iconSource: "qrc:/icons/reject.svg"
            actionEnabled: root.hasWorkspace
                && root.workspace.canMutateDecision
            onActivated: {
                if (!root.hasWorkspace)
                    return
                root.workspace.setSelectedFlag(
                    root.photoDecisionFlag === "rejected" ? "unflagged" : "rejected")
                root.close()
            }
        }

        MenuRow {
            text: qsTr("Rating")
            iconSource: "qrc:/icons/star.svg"
            actionEnabled: root.hasWorkspace
                && root.workspace.canMutateDecision
            expandable: true
            expanded: root.ratingsExpanded
            onActivated: root.ratingsExpanded = !root.ratingsExpanded
        }

        Column {
            visible: root.ratingsExpanded
            width: parent.width
            spacing: 1

            Repeater {
                model: 5

                delegate: MenuRow {
                    required property int index
                    indent: 12
                    text: qsTr("%1 star").arg(index + 1)
                    iconSource: "qrc:/icons/star.svg"
                    actionEnabled: root.hasWorkspace
                        && root.workspace.canMutateDecision
                    onActivated: {
                        if (!root.hasWorkspace)
                            return
                        root.workspace.setSelectedRating(index + 1)
                        root.close()
                    }
                }
            }
        }

        Divider {}

        MenuRow {
            text: qsTr("Add to Album")
            iconSource: "qrc:/icons/add-folder.svg"
            actionEnabled: root.hasWorkspace
                && !root.workspace.selectionContainsRemote()
                && root.workspace.manualLibraryAlbums.length > 0
                && !root.workspace.controller.libraryAlbumsBusy
            expandable: true
            expanded: root.albumsExpanded
            onActivated: root.albumsExpanded = !root.albumsExpanded
        }

        Column {
            visible: root.albumsExpanded
            width: parent.width
            spacing: 1

            Repeater {
                model: root.hasWorkspace
                    ? root.workspace.manualLibraryAlbums : []

                delegate: MenuRow {
                    required property var modelData
                    indent: 12
                    text: String(modelData.name)
                    iconSource: "qrc:/icons/library-manage.svg"
                    actionEnabled: root.hasWorkspace
                        && !root.workspace.selectionContainsRemote()
                        && !root.workspace.controller.libraryAlbumsBusy
                    onActivated: {
                        if (!root.hasWorkspace)
                            return
                        root.workspace.controller.addPhotosToManualLibraryAlbum(
                            String(modelData.id), root.workspace.batchSelectionTargets())
                        root.close()
                    }
                }
            }
        }

        MenuRow {
            text: qsTr("Apply Shared Node")
            iconSource: "qrc:/icons/shared-link.svg"
            actionEnabled: root.hasWorkspace
                && !root.workspace.selectionContainsRemote()
                && root.workspace.sharedNodeQuickList().length > 0
                && root.workspace.selectedPhotoCount > 0
            expandable: true
            expanded: root.nodesExpanded
            onActivated: root.nodesExpanded = !root.nodesExpanded
        }

        Column {
            visible: root.nodesExpanded
            width: parent.width
            spacing: 1

            Repeater {
                model: root.hasWorkspace
                    ? root.workspace.sharedNodeQuickList() : []

                delegate: MenuRow {
                    required property var modelData
                    indent: 12
                    text: String(modelData.label)
                    iconSource: "qrc:/icons/shared-link.svg"
                    actionEnabled: root.hasWorkspace
                        && !root.workspace.selectionContainsRemote()
                        && root.workspace.selectedPhotoCount > 0
                    onActivated: {
                        if (!root.hasWorkspace)
                            return
                        root.workspace.controller.applySharedGradeNode(
                            String(modelData.layerId),
                            root.workspace.batchSelectionTargets())
                        root.close()
                    }
                }
            }

            MenuRow {
                visible: root.hasWorkspace
                    && root.workspace.hasMoreSharedNodes()
                indent: 12
                text: qsTr("More shared nodes…")
                iconSource: "qrc:/icons/shared-link.svg"
                actionEnabled: root.hasWorkspace
                    && !root.workspace.selectionContainsRemote()
                    && root.workspace.selectedPhotoCount > 0
                onActivated: {
                    if (!root.hasWorkspace)
                        return
                    root.workspace.openSharedNodePicker(root.x, root.y)
                    root.close()
                }
            }
        }

        Divider {}

        MenuRow {
            text: qsTr("Export photo")
            iconSource: "qrc:/icons/export.svg"
            actionEnabled: root.hasWorkspace
                && !root.workspace.selectionContainsRemote()
                && root.workspace.selectedPhotoCount > 0
            onActivated: {
                if (!root.hasWorkspace)
                    return
                root.workspace.exportRequested(root.workspace.batchSelectionTargets())
                root.close()
            }
        }
    }
}
