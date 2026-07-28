pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property var controller
    required property var justifiedReviewLayout
    required property var editor
    required property var exportController
    required property var cacheMaintenanceController
    required property var preferences
    required property var lutLibrary
    required property var opticsProfileLibrary
    property int workspaceIndex: 0

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
    readonly property string descriptiveTitle: workspaceIndex === 0
        ? qsTr("Shadow · Review")
        : workspaceIndex === 1
            ? qsTr("Shadow · Precision") : qsTr("Shadow · Library")
    // macOS would otherwise draw a second native title beside our integrated
    // navigation. Mission Control and the Dock still receive the app identity.
    title: Qt.platform.os === "osx" ? "" : descriptiveTitle

    readonly property color panel: Theme.panel
    readonly property color panelRaised: Theme.panelRaised
    readonly property color border: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent

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

    Component.onCompleted: synchronizeTheme()

    Connections {
        target: window.preferences

        function onAppearanceModeChanged() {
            window.synchronizeTheme()
        }

        function onEffectiveAppearanceChanged() {
            window.synchronizeTheme()
        }
    }

    PreferencesMenu {
        id: preferencesMenu
        preferences: window.preferences
        onOpenLutLibraryRequested: window.openLutManager()
        onOpenCacheMaintenanceRequested: window.openCacheMaintenance()
    }

    LutManagerWindow {
        id: lutManager
        lutLibrary: window.lutLibrary
    }

    CacheMaintenanceWindow {
        id: cacheMaintenanceWindow
        cacheMaintenanceController: window.cacheMaintenanceController
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

    function openLutManager() {
        lutManager.openManager()
    }

    function openCacheMaintenance() {
        cacheMaintenanceWindow.present()
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
        if (workspaceIndex === 1)
            editor.closePhoto()
        workspaceIndex = workspace
    }

    function showReview() {
        leavePrecision(0)
    }

    function showPrecision() {
        if (workspaceIndex === 0 && reviewWorkspace.canOpenSelectedPhoto) {
            if (editor.active
                    && editor.photoId === reviewWorkspace.selectedPhotoId
                    && editor.representationId
                        === reviewWorkspace.selectedRepresentationId
                    && editor.sourcePath === reviewWorkspace.selectedPath) {
                reviewWorkspace.precisionOpenStatus = ""
                workspaceIndex = 1
                return
            }
            reviewWorkspace.openSelectedPhoto()
            return
        }
        if (editor.active || editor.busy)
            workspaceIndex = 1
    }

    function showLibrary() {
        leavePrecision(2)
    }

    function colorLabelName(label) {
        switch (String(label).toLowerCase()) {
        case "red": return qsTr("Red")
        case "yellow": return qsTr("Yellow")
        case "green": return qsTr("Green")
        case "blue": return qsTr("Blue")
        case "purple": return qsTr("Purple")
        default: return ""
        }
    }

    function filterFlagToolTip(flag) {
        switch (String(flag).toLowerCase()) {
        case "unflagged": return qsTr("Filter unflagged photos")
        case "picked": return qsTr("Filter flagged photos")
        case "rejected": return qsTr("Filter rejected photos")
        default: return qsTr("Clear all Library filters")
        }
    }

    function fullResolutionPreparationText() {
        const path = String(editor.sourcePath).toLowerCase()
        return /\.(jpe?g|heic|heif)$/.test(path)
            ? qsTr("Loading full-resolution image…")
            : qsTr("Parsing full-resolution RAW…")
    }

    function chooseLibraryFolder() {
        libraryFolderDialog.open()
    }

    function openPrecision(photoId, representationId, sourcePath, photoTitle, previewSource) {
        if (editor.active && editor.photoId === photoId
                && editor.representationId === representationId
                && editor.sourcePath === sourcePath) {
            reviewWorkspace.precisionOpenStatus = ""
            workspaceIndex = 1
            return
        }
        if (editor.openPhoto(photoId, representationId, sourcePath, photoTitle,
                             previewSource || "")) {
            reviewWorkspace.precisionOpenStatus = ""
            workspaceIndex = 1
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
        preferencesMenu: preferencesMenu
        workspaceIndex: window.workspaceIndex
        descriptiveTitle: window.descriptiveTitle
        canOpenSelectedPhoto: reviewWorkspace.canOpenSelectedPhoto
        onReviewRequested: window.showReview()
        onPrecisionRequested: window.showPrecision()
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
            preferences: window.preferences
            onExportRequested: targets => exportDialog.present(targets)
            onOpenPrecisionRequested: (photoId, representationId, sourcePath, photoTitle,
                                        previewSource) => {
                window.openPrecision(photoId, representationId, sourcePath, photoTitle,
                                     previewSource)
            }
            onOpenLibraryManagementRequested: window.showLibrary()
        }

        PrecisionWorkspace {
            id: precisionWorkspace
            objectName: "precisionWorkspace"
            Layout.fillWidth: true
            Layout.fillHeight: true
            editor: window.editor
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
                focalLengthMm: reviewWorkspace.selectedFocalLengthMm
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
    }

    footer: Rectangle {
        height: window.workspaceIndex === 0 ? 40 : 30
        color: Theme.chrome
        border.color: window.border

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 7

            BusyIndicator {
                Layout.preferredWidth: 15
                Layout.preferredHeight: 15
                visible: window.workspaceIndex !== 1
                    ? window.controller.scanning || window.controller.refreshing
                        || window.controller.busy
                        || window.controller.loadingMore
                        || window.controller.comparisonBusy
                        || window.controller.decisionBusy
                    : window.editor.busy
                        || window.editor.fullResolutionPreparing
                running: visible
            }

            Rectangle {
                visible: window.workspaceIndex === 0
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: 28
                implicitWidth: filterControls.implicitWidth + 12
                radius: 7
                color: window.controller.filterFlag !== "all"
                    || window.controller.filterMinimumRating > 0
                    || window.controller.filterColorLabel !== "all"
                    || window.controller.filterEditState !== "all"
                    || window.controller.filterLiked !== "all"
                    ? Theme.accentSurfaceQuiet : Theme.surfaceSubtle

                Row {
                    id: filterControls
                    anchors.centerIn: parent
                    spacing: 2

                    ShadowIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        source: "qrc:/icons/filter.svg"
                        size: 14
                        color: Theme.textMuted
                    }

                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("FILTER")
                        color: Theme.textMuted
                        font.pixelSize: 9
                        font.letterSpacing: 0.7
                    }

                    ShadowIconButton {
                        buttonSize: 24
                        iconSize: 14
                        source: "qrc:/icons/clear.svg"
                        selected: window.controller.filterFlag === "all"
                            && window.controller.filterMinimumRating === 0
                            && window.controller.filterColorLabel === "all"
                            && window.controller.filterEditState === "all"
                            && window.controller.filterLiked === "all"
                        toolTipText: qsTr("Clear all Library filters")
                        accessibleName: toolTipText
                        onClicked: window.controller.clearFilters()
                    }

                    Repeater {
                        model: ListModel {
                            ListElement { filterValue: "unflagged"; iconSource: "qrc:/icons/unflag.svg" }
                            ListElement { filterValue: "picked"; iconSource: "qrc:/icons/pick.svg" }
                            ListElement { filterValue: "rejected"; iconSource: "qrc:/icons/reject.svg" }
                        }

                        delegate: ShadowIconButton {
                            required property string filterValue
                            required property url iconSource
                            buttonSize: 24
                            iconSize: 15
                            source: iconSource
                            selected: window.controller.filterFlag === filterValue
                            toolTipText: window.filterFlagToolTip(filterValue)
                            accessibleName: toolTipText
                            onClicked: window.controller.filterFlag = selected
                                ? "all" : filterValue
                        }
                    }

                    Rectangle {
                        width: 1
                        height: 16
                        anchors.verticalCenter: parent.verticalCenter
                        color: window.border
                    }

                    Repeater {
                        model: 5

                        delegate: ShadowIconButton {
                            required property int index
                            buttonSize: 24
                            iconSize: 15
                            source: "qrc:/icons/star-filled.svg"
                            selected: window.controller.filterMinimumRating
                                === index + 1
                            foregroundColor: window.controller.filterMinimumRating
                                >= index + 1 ? Theme.labelYellow : Theme.textMuted
                            toolTipText: qsTr("Filter %L1 stars and above")
                                .arg(index + 1)
                            accessibleName: toolTipText
                            onClicked: window.controller.filterMinimumRating
                                = selected ? 0 : index + 1
                        }
                    }

                    Rectangle {
                        width: 1
                        height: 16
                        anchors.verticalCenter: parent.verticalCenter
                        color: window.border
                    }

                    ShadowIconButton {
                        buttonSize: 24
                        iconSize: 15
                        source: "qrc:/icons/heart-filled.svg"
                        selected: window.controller.filterLiked === "liked"
                        foregroundColor: selected ? Theme.accent : Theme.textMuted
                        toolTipText: qsTr("Filter liked photos")
                        accessibleName: toolTipText
                        onClicked: window.controller.filterLiked = selected
                            ? "all" : "liked"
                    }

                    Rectangle {
                        width: 1
                        height: 16
                        anchors.verticalCenter: parent.verticalCenter
                        color: window.border
                    }

                    ShadowIconButton {
                        buttonSize: 24
                        iconSize: 15
                        source: "qrc:/icons/edit.svg"
                        selected: window.controller.filterEditState === "edited"
                        toolTipText: qsTr("Filter edited photos")
                        accessibleName: toolTipText
                        onClicked: window.controller.filterEditState = selected
                            ? "all" : "edited"
                    }

                    ShadowIconButton {
                        buttonSize: 24
                        iconSize: 15
                        source: "qrc:/icons/edit-off.svg"
                        selected: window.controller.filterEditState === "unedited"
                        toolTipText: qsTr("Filter unedited photos")
                        accessibleName: toolTipText
                        onClicked: window.controller.filterEditState = selected
                            ? "all" : "unedited"
                    }

                    Rectangle {
                        width: 1
                        height: 16
                        anchors.verticalCenter: parent.verticalCenter
                        color: window.border
                    }

                    Repeater {
                        model: ["red", "yellow", "green", "blue", "purple"]

                        delegate: ShadowColorLabelButton {
                            required property string modelData
                            labelColor: Theme.colorLabel(modelData)
                            selected: window.controller.filterColorLabel === modelData
                            toolTipText: qsTr("Filter %1 color label").arg(
                                window.colorLabelName(modelData)
                            )
                            accessibleName: toolTipText
                            onClicked: window.controller.filterColorLabel = selected
                                ? "all" : modelData
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: window.workspaceIndex === 0
                    ? qsTr("%L1 / %L2 photos").arg(
                        window.controller.filteredItemCount
                    ).arg(window.controller.itemCount)
                    : window.workspaceIndex !== 1
                    ? (window.controller.decisionBusy
                        ? window.controller.decisionStatusText
                        : window.controller.comparisonBusy
                        ? window.controller.comparisonStatusText
                        : window.controller.statusText)
                    : window.editor.fullResolutionPreparing
                        ? window.fullResolutionPreparationText()
                        : window.editor.statusText
                color: window.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
            }

            Rectangle {
                visible: window.workspaceIndex === 1 && window.editor.active
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 1
                Layout.preferredHeight: 16
                color: window.border
            }

            RowLayout {
                visible: window.workspaceIndex === 1 && window.editor.active
                Layout.alignment: Qt.AlignVCenter
                spacing: 5

                Label {
                    text: qsTr("PROXY")
                    color: window.textMuted
                    font.pixelSize: 9
                    font.letterSpacing: 0.7
                }

                Rectangle {
                    Layout.preferredWidth: 6
                    Layout.preferredHeight: 6
                    radius: 3
                    color: precisionWorkspace.proxyActive
                        ? Theme.accent : Theme.textMuted
                }

                Label {
                    text: precisionWorkspace.proxyActive
                        ? qsTr("ON") : qsTr("OFF")
                    color: precisionWorkspace.proxyActive
                        ? Theme.accent : window.textMuted
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                }
            }

            Rectangle {
                visible: window.workspaceIndex === 0
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: 28
                implicitWidth: selectionControls.implicitWidth + 12
                radius: 7
                color: reviewWorkspace.canMutateDecision
                    ? Theme.accentSurfaceQuiet : Theme.buttonDisabledGhostSurface

                Row {
                    id: selectionControls
                    anchors.centerIn: parent
                    spacing: 2

                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: reviewWorkspace.canMutateDecision
                            ? qsTr("SELECTED") : qsTr("NO SELECTION")
                        color: reviewWorkspace.canMutateDecision
                            ? Theme.accentTextMuted : Theme.textDisabledQuiet
                        font.pixelSize: 9
                        font.letterSpacing: 0.7
                    }

                    Row {
                        visible: reviewWorkspace.canMutateDecision
                        spacing: 2

                        ShadowIconButton {
                            buttonSize: 24
                            iconSize: 14
                            source: "qrc:/icons/undo.svg"
                            toolTipText: qsTr("Undo the last decision")
                            accessibleName: toolTipText
                            enabled: window.controller.canUndoDecision
                            onClicked: window.controller.undoLastDecision()
                        }

                        Rectangle {
                            width: 1
                            height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            color: window.border
                        }

                        Repeater {
                            model: ListModel {
                                ListElement { actionId: "none"; flagValue: "unflagged"; iconSource: "qrc:/icons/unflag.svg" }
                                ListElement { actionId: "pick"; flagValue: "picked"; iconSource: "qrc:/icons/pick.svg" }
                                ListElement { actionId: "reject"; flagValue: "rejected"; iconSource: "qrc:/icons/reject.svg" }
                            }

                            delegate: ShadowIconButton {
                                required property string flagValue
                                required property url iconSource
                                buttonSize: 24
                                iconSize: 15
                                source: iconSource
                                toolTipText: flagValue === "picked"
                                    ? qsTr("Mark as picked (P)")
                                    : flagValue === "rejected"
                                        ? qsTr("Mark as rejected (X)")
                                        : qsTr("Clear decision flag (U)")
                                accessibleName: toolTipText
                                selected: reviewWorkspace.selectedDecisionFlag === flagValue
                                selectedSurfaceColor: flagValue === "picked"
                                    ? Theme.successSurface
                                    : flagValue === "rejected"
                                        ? Theme.dangerSurface : Theme.accentSurface
                                selectedHoverSurfaceColor: selectedSurfaceColor
                                selectedPressedSurfaceColor: selectedSurfaceColor
                                selectedIconColor: flagValue === "picked"
                                    ? Theme.successText
                                    : flagValue === "rejected"
                                        ? Theme.dangerText : Theme.accent
                                onClicked: reviewWorkspace.setSelectedFlag(flagValue)
                            }
                        }

                        Rectangle {
                            width: 1
                            height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            color: window.border
                        }

                        ShadowIconButton {
                            buttonSize: 24
                            iconSize: 13
                            source: "qrc:/icons/clear.svg"
                            selected: reviewWorkspace.selectedDecisionRating === 0
                            toolTipText: qsTr("Clear rating (0)")
                            accessibleName: toolTipText
                            onClicked: reviewWorkspace.setSelectedRating(0)
                        }

                        Repeater {
                            model: 5

                            delegate: ShadowIconButton {
                                required property int index
                                buttonSize: 24
                                iconSize: 15
                                source: index < reviewWorkspace.selectedDecisionRating
                                    ? "qrc:/icons/star-filled.svg" : "qrc:/icons/star.svg"
                                foregroundColor: index < reviewWorkspace.selectedDecisionRating
                                    ? Theme.labelYellow : Theme.textMuted
                                toolTipText: qsTr("Set rating to %L1 stars (%L1)")
                                    .arg(index + 1)
                                accessibleName: toolTipText
                                onClicked: reviewWorkspace.setSelectedRating(index + 1)
                            }
                        }

                        Rectangle {
                            width: 1
                            height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            color: window.border
                        }

                        ShadowIconButton {
                            buttonSize: 24
                            iconSize: 13
                            source: "qrc:/icons/clear.svg"
                            selected: reviewWorkspace.selectedColorLabel === "none"
                            toolTipText: qsTr("Clear local color label")
                            accessibleName: toolTipText
                            onClicked: window.controller.setPhotoColorLabel(
                                reviewWorkspace.selectedPhotoId, "none")
                        }

                        Repeater {
                            model: ["red", "yellow", "green", "blue", "purple"]

                            delegate: ShadowColorLabelButton {
                                required property string modelData
                                labelColor: Theme.colorLabel(modelData)
                                selected: reviewWorkspace.selectedColorLabel === modelData
                                toolTipText: qsTr("Set %1 color label").arg(
                                    window.colorLabelName(modelData)
                                )
                                accessibleName: toolTipText
                                onClicked: window.controller.setPhotoColorLabel(
                                    reviewWorkspace.selectedPhotoId, modelData)
                            }
                        }
                    }
                }
            }

            Label {
                visible: window.workspaceIndex !== 0
                text: window.workspaceIndex === 1
                    ? qsTr("PRECISION")
                    : qsTr("LIBRARY")
                color: Theme.textFaint
                font.pixelSize: 9
                font.letterSpacing: 0.8
            }
        }
    }
}
