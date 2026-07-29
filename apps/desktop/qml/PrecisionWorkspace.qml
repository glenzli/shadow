pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Page-level composition only.  The canvas owns mutually dependent viewport
// interaction; the inspector owns adjustment controls; this workspace owns
// only tool exclusivity and cross-panel wiring.
Item {
    id: precision

    required property var editor
    required property var lutLibrary
    required property var captureMetadata
    signal openLutLibraryRequested()
    signal openOpticsProfileLibraryRequested()
    signal returnToReviewRequested()

    // Special canvas tools are mutually exclusive and page-scoped. They do
    // not belong to a Grade Node parameter section because each tool owns a
    // coordinated inspector, pointer mode, and direct-manipulation overlay.
    readonly property int toolNone: 0
    readonly property int toolMask: 1
    readonly property int toolCrop: 2
    readonly property int toolRepair: 3
    property int activeSpecialTool: toolNone
    // Zero means freeform. Positive values are output-space aspect locks used
    // by the crop overlay, never persisted as a second geometry authority.
    property real cropAspectRatioLock: 0

    readonly property bool proxyActive: editor.active
        && precisionCanvas.visiblePreviewSource.length > 0
        && !precisionCanvas.showingFullDetail
    // Page-level consumers (including the headless acceptance path) need the
    // displayed frame identity, not an implementation-specific canvas id.
    // Keep the canvas as the sole owner of Image readiness while exposing its
    // read-only result through the workspace composition boundary.
    readonly property string readyPreviewGeneration: precisionCanvas.readyPreviewGeneration
    readonly property bool previewFrameReady: precisionCanvas.previewFrameReady
    readonly property bool beforeFrameReady: precisionCanvas.beforeFrameReady
    readonly property color panel: Theme.panel
    readonly property color panelRaised: Theme.panelRaised
    readonly property color border: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textSecondary: Theme.textSecondary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent

    function setActiveSpecialTool(requestedTool) {
        const nextTool = activeSpecialTool === requestedTool
            ? toolNone : requestedTool

        editor.setPointColorPickerActive(false)
        editor.setWhiteBalancePickerActive(false)
        if (nextTool !== toolRepair)
            editor.setRetouchPickerActive(false)

        activeSpecialTool = nextTool
        editor.setCropToolActive(nextTool === toolCrop)
        if (nextTool === toolRepair)
            editor.setRetouchPickerActive(true)
    }

    function leaveSpecialTool() {
        if (activeSpecialTool === toolNone)
            return
        activeSpecialTool = toolNone
        editor.setCropToolActive(false)
        editor.setRetouchPickerActive(false)
    }

    Connections {
        target: precision.editor

        function onSourceIdentityChanged() {
            precision.leaveSpecialTool()
        }

        function onActiveChanged() {
            if (!precision.editor.active)
                precision.leaveSpecialTool()
        }
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: precision.visible && precision.editor.active
            && precision.editor.canUndo && !precision.editor.stateBusy
        onActivated: precision.editor.undo()
    }

    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: precision.visible && precision.editor.active
            && precision.editor.canRedo && !precision.editor.stateBusy
        onActivated: precision.editor.redo()
    }

    Shortcut {
        sequence: "Escape"
        enabled: precision.visible
            && precision.activeSpecialTool !== precision.toolNone
        onActivated: precision.leaveSpecialTool()
    }

    Shortcut {
        sequence: "Y"
        enabled: precision.visible && precision.editor.active
        onActivated: {
            if (precisionCanvas.comparisonActive)
                precisionCanvas.comparisonActive = false
            else
                precisionCanvas.activateComparison(precisionCanvas.comparisonMode)
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        PrecisionGradeNodePane {
            Layout.preferredWidth: Math.max(220, Math.min(252, precision.width * 0.19))
            Layout.fillHeight: true
            editor: precision.editor
            panel: precision.panel
            panelRaised: precision.panelRaised
            borderColor: precision.border
            textPrimary: precision.textPrimary
            textSecondary: precision.textSecondary
            textMuted: precision.textMuted
            accent: precision.accent
            onMaskToolRequested: {
                if (precision.activeSpecialTool !== precision.toolMask)
                    precision.setActiveSpecialTool(precision.toolMask)
            }
        }

        PrecisionCanvas {
            id: precisionCanvas
            Layout.fillWidth: true
            Layout.fillHeight: true
            editor: precision.editor
            activeToolMode: precision.activeSpecialTool
            cropAspectRatioLock: precision.cropAspectRatioLock
            onNeutralToolRequested: precision.leaveSpecialTool()
        }

        PrecisionInspector {
            Layout.preferredWidth: Math.max(304, Math.min(348, precision.width * 0.24))
            Layout.fillHeight: true
            editor: precision.editor
            activeToolMode: precision.activeSpecialTool
            cropAspectRatioLock: precision.cropAspectRatioLock
            lutLibrary: precision.lutLibrary
            captureMetadata: precision.captureMetadata
            displayedHistogram: precisionCanvas.displayedHistogram
            displayingBefore: precisionCanvas.displayingBefore
            readyPreviewGeneration: precisionCanvas.readyPreviewGeneration
            previewFrameReady: precisionCanvas.previewFrameReady
            comparisonActive: precisionCanvas.comparisonActive
            currentPhotoAspect: precisionCanvas.imagePixelWidth
                / Math.max(1, precisionCanvas.imagePixelHeight)
            workspaceWidth: precision.width
            panel: precision.panel
            panelRaised: precision.panelRaised
            panelBorder: precision.border
            textPrimary: precision.textPrimary
            textSecondary: precision.textSecondary
            textMuted: precision.textMuted
            accent: precision.accent
            onOpenLutLibraryRequested: precision.openLutLibraryRequested()
            onOpenOpticsProfileLibraryRequested:
                precision.openOpticsProfileLibraryRequested()
            onToolModeRequested: mode => precision.setActiveSpecialTool(mode)
            onCropAspectRatioRequested: ratio =>
                precision.cropAspectRatioLock = ratio
        }
    }

    RecipeRecoveryPopup {
        editor: precision.editor
        textPrimary: precision.textPrimary
        borderColor: precision.border
        panelRaised: precision.panelRaised
        errorBorder: Theme.errorBorder
        errorText: Theme.errorText
        onReturnToReviewRequested: precision.returnToReviewRequested()
    }
}
