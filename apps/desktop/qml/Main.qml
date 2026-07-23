pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property var controller
    required property var editor
    required property var preferences
    required property var lutLibrary
    property int workspaceIndex: 0
    property bool closeAfterAutosave: false

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
    }

    LutManagerWindow {
        id: lutManager
        lutLibrary: window.lutLibrary
    }

    OpticsProfileManagerWindow {
        id: opticsProfileManager
        editor: window.editor
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

    onClosing: close => {
        if (!closeAfterAutosave && !editor.prepareToClose()) {
            close.accepted = false
        }
    }

    Connections {
        target: window.editor

        function onCloseReady() {
            window.closeAfterAutosave = true
            Qt.callLater(window.close)
        }

        function onCloseSaveFailed() {
            // A durable save error must not trap the native close gesture in
            // an automatic retry loop. The draft is still alive in memory, so
            // make the consequence explicit and leave the recovery choice to
            // the photographer.
            window.closeAfterAutosave = false
            autosaveFailurePopup.openingPendingPhoto = false
            autosaveFailurePopup.open()
        }

        function onPhotoSwitchSaveFailed() {
            // The current photo's latest in-memory adjustments could not be
            // made durable. Keep the target photo queued and make the three
            // possible outcomes explicit instead of silently trapping the
            // photographer on the current image.
            autosaveFailurePopup.openingPendingPhoto = true
            autosaveFailurePopup.open()
        }
    }

    Popup {
        id: autosaveFailurePopup
        property bool openingPendingPhoto: false
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(470, parent.width - 48)
        padding: 0
        modal: true
        dim: true
        focus: true
        closePolicy: Popup.NoAutoClose

        background: Rectangle {
            radius: Theme.controlRadius + 2
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.errorBorder
        }

        contentItem: ColumnLayout {
            spacing: 0

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 22
                spacing: 10

                Label {
                    Layout.fillWidth: true
                    text: qsTr("AUTOSAVE FAILED")
                    color: Theme.errorText
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.1
                }

                Label {
                    Layout.fillWidth: true
                    text: autosaveFailurePopup.openingPendingPhoto
                          ? qsTr("Shadow could not save this photo’s latest working adjustments. The selected photo will remain unopened until you retry, keep editing, or open it without these unsaved changes.")
                          : qsTr("Shadow could not save the latest working adjustments locally. You can retry, keep editing, or quit without the unsaved changes.")
                    color: Theme.textPrimary
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                    lineHeight: 1.35
                }

                Label {
                    Layout.fillWidth: true
                    visible: window.editor.autosaveErrorText.length > 0
                    text: window.editor.autosaveErrorText
                    color: Theme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    lineHeight: 1.3
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.border
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                spacing: 8

                ShadowButton {
                    Layout.fillWidth: true
                    text: qsTr("KEEP EDITING")
                    variant: ShadowButton.Secondary
                    onClicked: {
                        if (autosaveFailurePopup.openingPendingPhoto)
                            window.editor.cancelPendingPhotoOpen()
                        autosaveFailurePopup.close()
                    }
                }

                ShadowButton {
                    Layout.fillWidth: true
                    text: qsTr("RETRY SAVE")
                    variant: ShadowButton.Primary
                    enabled: !window.editor.stateBusy
                    onClicked: {
                        const openingPendingPhoto = autosaveFailurePopup.openingPendingPhoto
                        autosaveFailurePopup.close()
                        window.editor.retryAutosave()
                        if (!openingPendingPhoto) {
                            // Re-enter the regular close path while the retry
                            // is in flight. A successful durable snapshot
                            // emits closeReady; another failure reopens this
                            // popup.
                            Qt.callLater(window.close)
                        }
                    }
                }

                ShadowButton {
                    Layout.fillWidth: true
                    text: autosaveFailurePopup.openingPendingPhoto
                          ? qsTr("OPEN WITHOUT SAVING")
                          : qsTr("QUIT WITHOUT SAVING")
                    variant: ShadowButton.Danger
                    enabled: !window.editor.stateBusy
                    onClicked: {
                        const openingPendingPhoto = autosaveFailurePopup.openingPendingPhoto
                        autosaveFailurePopup.close()
                        if (openingPendingPhoto) {
                            // This bypass intentionally discards only the
                            // in-memory draft, then continues with the photo
                            // the user selected. The last durable working
                            // snapshot remains in the Catalog.
                            window.editor.discardFailedAutosaveAndOpenPendingPhoto()
                        } else {
                            // This bypass is intentionally reachable only
                            // after a confirmed autosave error. Normal closes
                            // always take the durable-snapshot path above.
                            window.closeAfterAutosave = true
                            Qt.callLater(window.close)
                        }
                    }
                }
            }
        }
    }

    header: ToolBar {
        id: titleToolBar
        objectName: "titleToolBar"
        Accessible.name: window.descriptiveTitle
        implicitHeight: 44
        topPadding: 0
        // macOS aligns its native window controls with this 44px region in
        // the platform-specific title-bar adapter, so QML stays geometric.
        bottomPadding: 0
        leftPadding: Math.max(
            SafeArea.margins.left,
            Qt.platform.os === "osx"
                && window.visibility !== Window.FullScreen ? 96 : 16
        )
        rightPadding: Math.max(
            SafeArea.margins.right,
            Qt.platform.os === "windows" ? 152 : 16
        )

        background: Rectangle {
            color: Theme.chrome

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: window.border
            }
        }

        contentItem: Item {
            Item {
                anchors.fill: parent

                DragHandler {
                    target: null
                    acceptedButtons: Qt.LeftButton
                    onActiveChanged: {
                        if (active)
                            window.startSystemMove()
                    }
                }
            }

            Row {
                id: brandMark
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Label {
                    text: "SHADOW"
                    color: window.textPrimary
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    font.letterSpacing: 2.5
                }

                Rectangle {
                    width: 1
                    height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    color: window.border
                }
            }

            Row {
                id: workspaceTabs
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                height: titleToolBar.availableHeight
                spacing: 10

                ShadowTabButton {
                    id: reviewModeButton
                    height: parent.height
                    active: window.workspaceIndex === 0
                    iconSource: "qrc:/icons/review-grid.svg"
                    iconSize: 18
                    minimumTabWidth: 46
                    underlineInset: 22
                    underlineBottomMargin: -titleToolBar.bottomPadding
                    text: qsTr("REVIEW")
                    toolTipText: text
                    onClicked: window.showReview()
                }

                ShadowTabButton {
                    id: precisionModeButton
                    height: parent.height
                    active: window.workspaceIndex === 1
                    iconSource: "qrc:/icons/edit.svg"
                    iconSize: 18
                    minimumTabWidth: 46
                    underlineInset: 22
                    underlineBottomMargin: -titleToolBar.bottomPadding
                    text: qsTr("PRECISION")
                    toolTipText: text
                    enabled: window.editor.active || window.editor.busy
                        || reviewWorkspace.canOpenSelectedPhoto
                    onClicked: window.showPrecision()
                }
            }

            Row {
                id: titleActions
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                Row {
                    visible: window.workspaceIndex === 1 && window.editor.active
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    Rectangle {
                        width: 7
                        height: 7
                        anchors.verticalCenter: parent.verticalCenter
                        radius: width / 2
                        color: !window.editor.dirty ? Theme.savedText
                            : window.editor.autosaveFailed ? Theme.errorText : Theme.warningText
                    }

                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: !window.editor.dirty ? qsTr("SAVED")
                            : window.editor.autosaveFailed ? qsTr("SAVE FAILED")
                            : window.editor.autosavePending ? qsTr("SAVING") : qsTr("DRAFT")
                        color: !window.editor.dirty ? Theme.savedText
                            : window.editor.autosaveFailed ? Theme.errorText : Theme.warningText
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.65
                    }
                }

                Row {
                    visible: window.workspaceIndex === 1
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    ShadowIconButton {
                        id: globalUndoButton
                        source: "qrc:/icons/undo.svg"
                        toolTipText: qsTr("Undo")
                        accessibleName: toolTipText
                        enabled: window.editor.active && window.editor.canUndo
                            && !window.editor.stateBusy
                        onClicked: window.editor.undo()
                    }

                    ShadowIconButton {
                        id: globalRedoButton
                        source: "qrc:/icons/redo.svg"
                        toolTipText: qsTr("Redo")
                        accessibleName: toolTipText
                        enabled: window.editor.active && window.editor.canRedo
                            && !window.editor.stateBusy
                        onClicked: window.editor.redo()
                    }
                }

                ShadowIconButton {
                    id: libraryButton
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/icons/library-manage.svg"
                    text: qsTr("Library Management")
                    toolTipText: text
                    accessibleName: text
                    selected: window.workspaceIndex === 2
                    Accessible.checked: selected
                    variant: window.controller.scanning
                        || window.controller.refreshing
                        ? ShadowIconButton.Tinted : ShadowIconButton.Ghost
                    onClicked: window.showLibrary()
                }

                ShadowIconButton {
                    id: closeEditButton
                    visible: window.workspaceIndex === 1
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/icons/clear.svg"
                    text: qsTr("Return to Review")
                    toolTipText: text
                    accessibleName: text
                    onClicked: window.showReview()
                }

                ShadowIconButton {
                    id: settingsButton
                    objectName: "settingsButton"
                    anchors.verticalCenter: parent.verticalCenter
                    buttonSize: 28
                    source: "qrc:/icons/settings.svg"
                    text: qsTr("Settings")
                    toolTipText: text
                    accessibleName: text
                    onClicked: preferencesMenu.popup(
                        settingsButton,
                        settingsButton.width - preferencesMenu.width,
                        settingsButton.height + titleToolBar.bottomPadding + 4
                    )
                }
            }
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
            preferences: window.preferences
            onOpenPrecisionRequested: (photoId, representationId, sourcePath, photoTitle,
                                        previewSource) => {
                window.openPrecision(photoId, representationId, sourcePath, photoTitle,
                                     previewSource)
            }
        }

        PrecisionWorkspace {
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
                    : window.editor.statusText
                color: window.textMuted
                font.pixelSize: 10
                elide: Text.ElideRight
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
