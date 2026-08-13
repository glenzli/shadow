pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Application-level Map workspace. It consumes Review's authoritative
// selection and Library-scope state while owning map-only presentation,
// transient dialogs, and the default-collapsed scope navigation.
Item {
    id: mapWorkspace
    objectName: "libraryMapWorkspace"

    required property var libraryContext
    property bool libraryScopeExpanded: false
    readonly property bool nativeSurfaceBlocked:
        albumDialogs.modalVisible || keywordPopup.opened

    signal openLibraryManagementRequested()

    LibraryAlbumDialogs {
        id: albumDialogs
        anchors.fill: parent
        controller: mapWorkspace.libraryContext.controller
        hasActiveLibraryFilter:
            mapWorkspace.libraryContext.hasActiveLibraryFilter
        manualAlbums: mapWorkspace.libraryContext.manualLibraryAlbums
    }

    LibraryKeywordPopup {
        id: keywordPopup
        workspace: mapWorkspace.libraryContext
    }

    ReviewMetadataPresentation {
        id: metadataPresentation
        workspace: mapWorkspace.libraryContext
    }

    MetadataWindow {
        id: metadataWindow
        transientParent: mapWorkspace.Window.window
        preferences: mapWorkspace.libraryContext.preferences
        controller: mapWorkspace.libraryContext.controller
        photoTitle: mapWorkspace.libraryContext.selectedTitle
        photoId: mapWorkspace.libraryContext.selectedPhotoId
        selectionTargets: mapWorkspace.libraryContext.batchSelectionTargets()
        sourcePath: mapWorkspace.libraryContext.selectedPath
        hasMetadata: mapWorkspace.libraryContext.selectedHasMetadata
        metadataPending:
            mapWorkspace.libraryContext.controller.photoInspectionBusy
        metadataFailed:
            mapWorkspace.libraryContext.controller.photoInspectionFailed
        fields: metadataPresentation.metadataFields()
        onRetryRequested:
            mapWorkspace.libraryContext.controller.retryPhotoInspection()
    }

    LibraryLocationBatchDialog {
        id: locationBatchDialog
        transientParent: mapWorkspace.Window.window
        controller: mapWorkspace.libraryContext.controller
        mapController: mapWorkspace.libraryContext.libraryWebMapController
        placeSearchService: mapWorkspace.libraryContext.amapPlaceSearchService
        nativeWebMapAllowed: mapWorkspace.libraryContext.nativeLocationDialogWebMapAllowed
        onConfigureMapRequested:
            mapWorkspace.libraryContext.openMapProviderSettingsRequested()
    }

    LibraryLocationCompletionDialog {
        id: locationCompletionDialog
        transientParent: mapWorkspace.Window.window
        controller: mapWorkspace.libraryContext.controller
        onUseGroupRequested: (targets, hasSuggestion, latitude, longitude, placeName) => {
            locationBatchDialog.presentWithSource(
                targets, hasSuggestion, latitude, longitude, placeName,
                hasSuggestion ? "location-completion:event-anchor" : "manual-map")
        }
    }

    Connections {
        target: mapWorkspace.libraryContext

        function onLibraryScopeCommitted() {
            mapWorkspace.libraryScopeExpanded = false
        }
    }

    onVisibleChanged: {
        if (visible)
            libraryScopeExpanded = false
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ReviewLibrarySidebar {
            id: libraryScopeSidebar
            objectName: "mapLibraryScopeSidebar"
            visible: mapWorkspace.libraryScopeExpanded
            Layout.preferredWidth: visible ? 210 : 0
            workspace: mapWorkspace.libraryContext
            albumDialogs: albumDialogs
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 42
                color: Theme.chrome

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Theme.border
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 12
                    spacing: 8

                    ShadowIconButton {
                        objectName: "mapLibraryScopeButton"
                        source: "qrc:/icons/scopes.svg"
                        selected: mapWorkspace.libraryScopeExpanded
                        toolTipText: selected
                            ? qsTr("Hide Library scope selector")
                            : qsTr("Show Library scope selector")
                        accessibleName: toolTipText
                        onClicked:
                            mapWorkspace.libraryScopeExpanded = !selected
                    }

                    Label {
                        text: mapWorkspace.libraryContext
                            .currentLibraryScopeName.toUpperCase()
                        color: Theme.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                    }

                    Label {
                        text: qsTr("%L1 visible").arg(
                            mapWorkspace.libraryContext.controller
                                .filteredItemCount)
                        color: Theme.textPrimary
                        font.pixelSize: 11
                    }

                    Item { Layout.fillWidth: true }

                    ShadowIconButton {
                        source: "qrc:/icons/pin.svg"
                        toolTipText: qsTr("Complete missing photo locations")
                        accessibleName: toolTipText
                        onClicked: locationCompletionDialog.present()
                    }

                    ShadowIconButton {
                        checkable: true
                        checked: mapWorkspace.libraryContext
                            .hasLibraryKeywordFilter
                        source: "qrc:/icons/tag.svg"
                        toolTipText:
                            qsTr("Assign and filter Library keywords")
                        accessibleName: toolTipText
                        onClicked: keywordPopup.present()
                    }

                    ShadowIconButton {
                        source: "qrc:/icons/library-manage.svg"
                        toolTipText: qsTr("Manage photo sources")
                        accessibleName: toolTipText
                        onClicked:
                            mapWorkspace.openLibraryManagementRequested()
                    }
                }
            }

            LibraryMapView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                workspace: mapWorkspace.libraryContext
            }
        }

        ReviewPhotoInspector {
            Layout.preferredWidth: 278
            review: mapWorkspace.libraryContext
            metadataPresentation: metadataPresentation
            onOpenMetadataRequested: metadataWindow.present()
        }
    }
}
