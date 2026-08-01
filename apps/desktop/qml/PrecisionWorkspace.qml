pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// Page-level composition only.  The canvas owns mutually dependent viewport
// interaction; the inspector owns adjustment controls; this workspace owns
// only tool exclusivity and cross-panel wiring.
Item {
    id: precision

    required property var editor
    required property var editPreviewPresentation
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
    readonly property int toolLiquify: 4
    property int activeSpecialTool: toolNone
    property bool selectedRetouchContinuous: true
    property int selectedRetouchIndex: -1
    // Zero means freeform. Positive values are output-space aspect locks used
    // by the crop overlay, never persisted as a second geometry authority.
    property real cropAspectRatioLock: 0
    property string observedRecipeNodeKind: ""
    property string observedGradeNodeId: ""
    property int observedGradeNodeIndex: -1

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

        if (nextTool === toolCrop) {
            editor.addCanvasNode()
            if (!editor.canvasNodeMaterialized)
                return
            editor.selectCanvasNode()
        }

        editor.setPointColorPickerActive(false)
        editor.setWhiteBalancePickerActive(false)
        if (nextTool !== toolRepair)
            editor.setRetouchPickerActive(false)

        activeSpecialTool = nextTool
        editor.setMaskToolActive(nextTool === toolMask)
        editor.setCropToolActive(nextTool === toolCrop)
        if (nextTool === toolRepair)
            editor.setRetouchPickerActive(true)
    }

    function leaveSpecialTool() {
        if (activeSpecialTool === toolNone)
            return
        activeSpecialTool = toolNone
        editor.setMaskToolActive(false)
        editor.setCropToolActive(false)
        editor.setRetouchPickerActive(false)
    }

    function reconcileRecipeNodeSelection() {
        const nextKind = String(editor.selectedRecipeNodeKind || "")
        const nextId = String(editor.selectedGradeNodeId || "")
        const nextIndex = Number(editor.selectedGradeNodeIndex)
        const initialized = observedRecipeNodeKind.length > 0
            || observedGradeNodeId.length > 0
            || observedGradeNodeIndex >= 0
        const selectionChanged = initialized
            && (nextKind !== observedRecipeNodeKind
                || nextId !== observedGradeNodeId
                || nextIndex !== observedGradeNodeIndex)
        observedRecipeNodeKind = nextKind
        observedGradeNodeId = nextId
        observedGradeNodeIndex = nextIndex
        if (selectionChanged)
            leaveSpecialTool()
    }

    function cancelTransientInteractionOrLeaveTool() {
        if (editor.pointColorPickerActive) {
            editor.setPointColorPickerActive(false)
            return
        }
        if (editor.whiteBalancePickerActive) {
            editor.setWhiteBalancePickerActive(false)
            return
        }
        if (editor.retouchPickerActive) {
            editor.setRetouchPickerActive(false)
            return
        }
        leaveSpecialTool()
    }

    function selectRetouchRegion(continuous, index) {
        selectedRetouchContinuous = continuous
        selectedRetouchIndex = index
    }

    Connections {
        target: precision.editor

        function onSelectedGradeNodeChanged() {
            precision.reconcileRecipeNodeSelection()
        }

        function onSourceIdentityChanged() {
            precision.selectedRetouchContinuous = true
            precision.selectedRetouchIndex = -1
            precision.leaveSpecialTool()
        }

        function onActiveChanged() {
            if (!precision.editor.active)
                precision.leaveSpecialTool()
        }
    }

    Component.onCompleted: {
        observedRecipeNodeKind = String(editor.selectedRecipeNodeKind || "")
        observedGradeNodeId = String(editor.selectedGradeNodeId || "")
        observedGradeNodeIndex = Number(editor.selectedGradeNodeIndex)
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
        enabled: precision.visible && (precision.editor.pointColorPickerActive
            || precision.editor.whiteBalancePickerActive
            || precision.editor.retouchPickerActive
            || precision.activeSpecialTool !== precision.toolNone)
        onActivated: precision.cancelTransientInteractionOrLeaveTool()
    }

    Shortcut {
        sequence: "O"
        enabled: precision.visible
            && precision.activeSpecialTool === precision.toolMask
        onActivated:
            precisionCanvas.maskOverlayVisible =
                !precisionCanvas.maskOverlayVisible
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
            onCropToolRequested: {
                if (precision.activeSpecialTool !== precision.toolCrop)
                    precision.setActiveSpecialTool(precision.toolCrop)
            }
            onLiquifyToolRequested: {
                if (precision.activeSpecialTool !== precision.toolLiquify)
                    precision.setActiveSpecialTool(precision.toolLiquify)
            }
        }

        PrecisionCanvas {
            id: precisionCanvas
            Layout.fillWidth: true
            Layout.fillHeight: true
            editor: precision.editor
            editPreviewPresentation:
                precision.editPreviewPresentation
            captureMetadata: precision.captureMetadata
            activeToolMode: precision.activeSpecialTool
            cropAspectRatioLock: precision.cropAspectRatioLock
            selectedRetouchContinuous:
                precision.selectedRetouchContinuous
            selectedRetouchIndex: precision.selectedRetouchIndex
            onNeutralToolRequested: precision.leaveSpecialTool()
            onRetouchRegionSelectionRequested:
                (continuous, index) =>
                    precision.selectRetouchRegion(continuous, index)
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
            maskOverlayVisible: precisionCanvas.maskOverlayVisible
            selectedRetouchContinuous:
                precision.selectedRetouchContinuous
            selectedRetouchIndex: precision.selectedRetouchIndex
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
            onMaskOverlayVisibilityRequested: visible =>
                precisionCanvas.maskOverlayVisible = visible
            onRetouchRegionSelectionRequested:
                (continuous, index) =>
                    precision.selectRetouchRegion(continuous, index)
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
