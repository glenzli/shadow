pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property var controller
    required property var justifiedReviewLayout
    required property var reviewGalleryGrouping
    required property var editor
    required property var editInterchangeController
    required property var editPreviewPresentation
    required property var exportController
    required property var cacheMaintenanceController
    required property var peopleAnalysisController
    required property var semanticSearchController
    required property var smartCategoryController
    required property var imageUnderstandingController
    required property var aiPreferences
    required property var cachePreferences
    required property var historyController
    required property var preferences
    required property var personalProfile
    required property var personalLocationSearch
    required property var mapProviderPreferences
    required property var libraryWebMapController
    required property var amapPlaceSearchService
    required property var lutLibrary
    required property var opticsProfileLibrary
    required property string initialSettingsSection
    readonly property int reviewWorkspacePage: 0
    readonly property int precisionWorkspacePage: 1
    readonly property int libraryManagementPage: 2
    readonly property int peopleWorkspacePage: 3
    readonly property int mapWorkspacePage: 4
    property int workspaceIndex: reviewWorkspacePage

    width: 1480
    height: 920
    minimumWidth: 1200
    minimumHeight: 680
    // Keep the native frame and traffic-light controls, but let the app chrome
    // paint through the title-bar area. The ToolBar below consumes SafeArea
    // margins and delegates drags on its empty surface to the window manager.
    flags: Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
    visible: true
    color: Theme.window
    // Propagate the same design tokens into any remaining Qt Quick Control
    // that has not yet been promoted to a Shadow semantic component.
    palette.window: Theme.window
    palette.windowText: Theme.textPrimary
    palette.base: Theme.panelRaised
    palette.alternateBase: Theme.panel
    palette.text: Theme.textPrimary
    palette.button: Theme.buttonSurface
    palette.buttonText: Theme.textPrimary
    palette.mid: Theme.border
    palette.dark: Theme.borderStrong
    palette.light: Theme.panelRaised
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.selectionForeground
    palette.placeholderText: Theme.textPlaceholder
    palette.disabled.text: Theme.textDisabled
    palette.disabled.buttonText: Theme.textDisabled
    palette.disabled.button: Theme.buttonDisabledSurface
    readonly property string descriptiveTitle:
        workspaceIndex === reviewWorkspacePage
        ? qsTr("Shadow · Review")
        : workspaceIndex === precisionWorkspacePage
            ? qsTr("Shadow · Precision")
            : workspaceIndex === mapWorkspacePage
                ? qsTr("Shadow · Map")
                : workspaceIndex === peopleWorkspacePage
                    ? qsTr("Shadow · People")
                    : qsTr("Shadow · Library")
    // macOS would otherwise draw a second native title beside our integrated
    // navigation. Mission Control and the Dock still receive the app identity.
    title: Qt.platform.os === "osx" ? "" : descriptiveTitle

    readonly property color panel: Theme.panel
    readonly property color panelRaised: Theme.panelRaised
    readonly property color border: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent
    readonly property bool modalSurfaceClear:
        !applicationSettingsDialog.opened
        && !personalProfileDialog.opened
        && !exportDialog.opened
        && !historyDrawer.opened
    readonly property bool nativeWebMapForegroundAllowed:
        window.workspaceIndex === mapWorkspacePage
        && modalSurfaceClear
    readonly property bool nativeLocationDialogWebMapAllowed:
        window.workspaceIndex === reviewWorkspacePage
        && modalSurfaceClear
    readonly property bool mapNativeSurfaceBlocked:
        mapWorkspace.nativeSurfaceBlocked

    function synchronizeTheme() {
        const configuredMode = String(preferences.appearanceMode)
        if (configuredMode === "light")
            Theme.mode = Theme.Light
        else if (configuredMode === "dark")
            Theme.mode = Theme.Dark
        else
            Theme.mode = Theme.System

        const effectiveAppearance = String(preferences.effectiveAppearance)
        Theme.effectiveDark = effectiveAppearance === "dark"
            ? true
            : effectiveAppearance === "light" ? false : Boolean(preferences.dark)
    }

    Component.onCompleted: {
        synchronizeTheme()
        if (initialSettingsSection.length > 0) {
            Qt.callLater(function() {
                applicationSettingsDialog.present(initialSettingsSection)
            })
        }
    }

    Connections {
        target: window.preferences

        function onAppearanceModeChanged() {
            window.synchronizeTheme()
        }

        function onEffectiveAppearanceChanged() {
            window.synchronizeTheme()
        }
    }

    ApplicationSettingsDialog {
        id: applicationSettingsDialog
        preferences: window.preferences
        aiPreferences: window.aiPreferences
        peopleAnalysisController: window.peopleAnalysisController
        imageUnderstandingController: window.imageUnderstandingController
        cachePreferences: window.cachePreferences
        cacheMaintenanceController: window.cacheMaintenanceController
        mapProviderPreferences: window.mapProviderPreferences
        editor: window.editor
        hostWidth: window.width
        hostHeight: window.height
        onOpenLutLibraryRequested: {
            close()
            window.openLutManager()
        }
        onOpenOpticsProfileLibraryRequested: {
            close()
            window.openOpticsProfileManager()
        }
    }

    PersonalProfileDialog {
        id: personalProfileDialog
        profile: window.personalProfile
        controller: window.controller
        locationSearch: window.personalLocationSearch
        hostWidth: window.width
        hostHeight: window.height
    }

    LutManagerWindow {
        id: lutManager
        lutLibrary: window.lutLibrary
    }

    OpticsProfileManagerWindow {
        id: opticsProfileManager
        editor: window.editor
        opticsProfileLibrary: window.opticsProfileLibrary
    }

    ExportDialog {
        id: exportDialog
        exportController: window.exportController
    }

    HistoryDrawer {
        id: historyDrawer
        historyController: window.historyController
        editor: window.editor
        hostWindow: window
    }

    function openLutManager() {
        lutManager.openManager()
    }

    function openOpticsProfileManager() {
        opticsProfileManager.openManager()
    }

    FolderDialog {
        id: libraryFolderDialog
        title: qsTr("Choose a photo folder")
        onAccepted: window.controller.scanFolder(selectedFolder)
    }

    function leavePrecision(workspace) {
        // Precision owns an asynchronous edit session. Merely hiding its
        // StackLayout page left the old source active, so a subsequent grid
        // open could appear to reopen the previous photo. Close the session
        // first; its autosave path is non-blocking and safely chains a later
        // selection if the user immediately opens another item.
        if (workspaceIndex === precisionWorkspacePage)
            editor.closePhoto()
        workspaceIndex = workspace
    }

    function showReview() {
        leavePrecision(reviewWorkspacePage)
    }

    function showPrecision() {
        if ((workspaceIndex === reviewWorkspacePage
                || workspaceIndex === mapWorkspacePage)
                && reviewWorkspace.canOpenSelectedPhoto) {
            if (editor.active
                    && editor.photoId === reviewWorkspace.selectedPhotoId
                    && editor.representationId
                        === reviewWorkspace.selectedRepresentationId
                    && editor.sourcePath === reviewWorkspace.selectedPath) {
                reviewWorkspace.precisionOpenStatus = ""
                workspaceIndex = precisionWorkspacePage
                return
            }
            reviewWorkspace.openSelectedPhoto()
            return
        }
        if (editor.active || editor.busy)
            workspaceIndex = precisionWorkspacePage
    }

    function showLibrary() {
        leavePrecision(libraryManagementPage)
    }

    function showPeople() {
        leavePrecision(peopleWorkspacePage)
    }

    function showMap() {
        leavePrecision(mapWorkspacePage)
    }

    function chooseLibraryFolder() {
        libraryFolderDialog.open()
    }

    function openPrecision(photoId, representationId, sourcePath, photoTitle, previewSource) {
        if (editor.active && editor.photoId === photoId
                && editor.representationId === representationId
                && editor.sourcePath === sourcePath) {
            reviewWorkspace.precisionOpenStatus = ""
            workspaceIndex = precisionWorkspacePage
            return
        }
        if (editor.openPhoto(photoId, representationId, sourcePath, photoTitle,
                             previewSource || "")) {
            reviewWorkspace.precisionOpenStatus = ""
            workspaceIndex = precisionWorkspacePage
        } else {
            reviewWorkspace.reportPrecisionOpenFailure(editor.statusText)
        }
    }

    AutosaveFailureRecovery {
        parent: Overlay.overlay
        editor: window.editor
        hostWindow: window
    }

    Connections {
        target: window.editor

        function onActiveChanged() {
            if (!window.editor.active)
                window.controller.refreshVisibleLibrary()
        }
    }

    header: MainTitleBar {
        hostWindow: window
        editor: window.editor
        settingsDialog: applicationSettingsDialog
        personalProfile: window.personalProfile
        personalProfileDialog: personalProfileDialog
        workspaceIndex: window.workspaceIndex
        reviewWorkspaceIndex: window.reviewWorkspacePage
        precisionWorkspaceIndex: window.precisionWorkspacePage
        mapWorkspaceIndex: window.mapWorkspacePage
        peopleWorkspaceIndex: window.peopleWorkspacePage
        descriptiveTitle: window.descriptiveTitle
        canOpenSelectedPhoto: reviewWorkspace.canOpenSelectedPhoto
        historyOpen: historyDrawer.opened
        onReviewRequested: window.showReview()
        onPrecisionRequested: window.showPrecision()
        onMapRequested: window.showMap()
        onPeopleRequested: window.showPeople()
        onHistoryRequested: {
            if (historyDrawer.opened) {
                historyDrawer.close()
                return
            }
            const photoId = window.editor.active
                ? window.editor.photoId : reviewWorkspace.selectedPhotoId
            const title = window.editor.active
                ? window.editor.title : reviewWorkspace.selectedTitle
            historyDrawer.present(photoId, title)
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: window.workspaceIndex

        ReviewWorkspace {
            id: reviewWorkspace
            Layout.fillWidth: true
            Layout.fillHeight: true
            controller: window.controller
            justifiedReviewLayout: window.justifiedReviewLayout
            reviewGalleryGrouping: window.reviewGalleryGrouping
            preferences: window.preferences
            mapProviderPreferences: window.mapProviderPreferences
            libraryWebMapController: window.libraryWebMapController
            amapPlaceSearchService: window.amapPlaceSearchService
            personalProfile: window.personalProfile
            semanticSearchController: window.semanticSearchController
            smartCategoryController: window.smartCategoryController
            imageUnderstandingController: window.imageUnderstandingController
            // Qt WebView is a native child view on macOS rather than a scene-
            // graph item. It must not remain materialized beneath a QML modal,
            // because the native layer would cover the modal regardless of its
            // QML z-order.
            nativeWebMapAllowed: window.nativeWebMapForegroundAllowed
                && !window.mapNativeSurfaceBlocked
            nativeLocationDialogWebMapAllowed:
                window.nativeLocationDialogWebMapAllowed
            onExportRequested: targets => exportDialog.present(targets)
            onOpenPrecisionRequested: (photoId, representationId, sourcePath, photoTitle,
                                        previewSource) => {
                window.openPrecision(photoId, representationId, sourcePath, photoTitle,
                                     previewSource)
            }
            onOpenLibraryManagementRequested: window.showLibrary()
            onOpenMapProviderSettingsRequested:
                applicationSettingsDialog.present("maps")
        }

        PrecisionWorkspace {
            id: precisionWorkspace
            objectName: "precisionWorkspace"
            Layout.fillWidth: true
            Layout.fillHeight: true
            editor: window.editor
            interchangeController: window.editInterchangeController
            editPreviewPresentation: window.editPreviewPresentation
            lutLibrary: window.lutLibrary
            captureMetadata: ({
                representationId: reviewWorkspace.selectedRepresentationId,
                pending: window.controller.scanning || window.controller.refreshing,
                available: reviewWorkspace.selectedHasMetadata,
                cameraMake: reviewWorkspace.selectedCameraMake,
                cameraModel: reviewWorkspace.selectedCameraModel,
                lensMake: reviewWorkspace.selectedLensMake,
                lensModel: reviewWorkspace.selectedLensModel,
                isoSpeed: reviewWorkspace.selectedIsoSpeed,
                exposureTimeSeconds: reviewWorkspace.selectedExposureTimeSeconds,
                apertureFNumber: reviewWorkspace.selectedApertureFNumber,
                focalLengthMm: reviewWorkspace.selectedFocalLengthMm,
                hasFocusObservation:
                    reviewWorkspace.selectedHasFocusObservation,
                focusObservationSchemaVersion:
                    reviewWorkspace.selectedFocusObservationSchemaVersion,
                focusObservationSource:
                    reviewWorkspace.selectedFocusObservationSource,
                focusObservationCenterX:
                    reviewWorkspace.selectedFocusObservationCenterX,
                focusObservationCenterY:
                    reviewWorkspace.selectedFocusObservationCenterY,
                focusObservationWidth:
                    reviewWorkspace.selectedFocusObservationWidth,
                focusObservationHeight:
                    reviewWorkspace.selectedFocusObservationHeight,
                focusObservationConfirmed:
                    reviewWorkspace.selectedFocusObservationConfirmed,
                focusObservationConfidence:
                    reviewWorkspace.selectedFocusObservationConfidence
            })
            onOpenLutLibraryRequested: window.openLutManager()
            onOpenOpticsProfileLibraryRequested: window.openOpticsProfileManager()
            onReturnToReviewRequested: window.showReview()
        }

        LibraryWorkspace {
            Layout.fillWidth: true
            Layout.fillHeight: true
            controller: window.controller
            onChooseFolderRequested: window.chooseLibraryFolder()
        }

        PeopleWorkspace {
            Layout.fillWidth: true
            Layout.fillHeight: true
            controller: window.peopleAnalysisController
            aiPreferences: window.aiPreferences
        }

        LibraryMapWorkspace {
            id: mapWorkspace
            Layout.fillWidth: true
            Layout.fillHeight: true
            libraryContext: reviewWorkspace
            onOpenLibraryManagementRequested: window.showLibrary()
        }
    }

    footer: MainStatusBar {
        workspaceIndex: window.workspaceIndex
        reviewWorkspaceIndex: window.reviewWorkspacePage
        precisionWorkspaceIndex: window.precisionWorkspacePage
        mapWorkspaceIndex: window.mapWorkspacePage
        peopleWorkspaceIndex: window.peopleWorkspacePage
        controller: window.controller
        editor: window.editor
        peopleAnalysisController: window.peopleAnalysisController
        semanticSearchController: window.semanticSearchController
        reviewWorkspace: reviewWorkspace
        precisionWorkspace: precisionWorkspace
    }
}
