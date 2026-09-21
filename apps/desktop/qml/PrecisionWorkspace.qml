pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// Page-level composition only.  The canvas owns mutually dependent viewport
// interaction; the inspector owns adjustment controls; this workspace owns
// only tool exclusivity and cross-panel wiring.
Item {
    id: precision

    required property var editor
    required property var interchangeController
    property var lutExportController: null
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
    readonly property int toolCompletion: 5
    readonly property int toolPaint: 6
    property int activeSpecialTool: toolNone
    property bool selectedRetouchContinuous: true
    property int selectedRetouchIndex: -1
    // Zero means freeform. Positive values are output-space aspect locks used
    // by the crop overlay, never persisted as a second geometry authority.
    property real cropAspectRatioLock: 0
    property string observedRecipeNodeKind: ""
    property string observedGradeNodeId: ""
    property int observedGradeNodeIndex: -1
    property string lastAdjustmentNodeId: ""
    property string observedVariantId: ""
    property bool colorWarperExpanded: false
    property real colorWarperPanelWidth: 520

    function openColorWarper() {
        if (!editor.active || !editor.gradeNodeEnabled)
            return
        leaveSpecialTool()
        if (editor.targetedCurve) editor.targetedCurve.active = false
        editor.setPointColorPickerActive(false)
        editor.setWhiteBalancePickerActive(false)
        colorWarperExpanded = true
        precisionCanvas.resetView()
    }

    function closeColorWarper() {
        if (!colorWarperExpanded)
            return
        colorWarperPanel.finishEditing()
        colorWarperExpanded = false
    }

    function restoreAdjustmentSelection() {
        if (editor.hasSelectedGradeNode || !editor.active)
            return
        const nodes = editor.gradeNodes
        let index = 0
        for (let i = 0; i < nodes.length; ++i) {
            if (String(nodes[i].gradeNodeId) === lastAdjustmentNodeId) {
                index = i
                break
            }
        }
        if (nodes.length > 0)
            editor.selectGradeNode(index)
    }

    readonly property bool proxyActive: editor.active
        && precisionCanvas.visiblePreviewSource.length > 0
        && !precisionCanvas.showingFullDetail
    // Idle full-resolution source preparation is deliberately background
    // work. It becomes foreground activity only while the user is actually
    // waiting on a 100% viewport or the explicit detail loupe.
    readonly property bool foregroundFullResolutionPending:
        editor.active && editor.fullResolutionPreparing
        && (precisionCanvas.detailLoupeVisible
            || (!precisionCanvas.fitView
                && precisionCanvas.zoomFactor >= 1.0
                && !precisionCanvas.comparisonActive
                && editor.detailMode))
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
        closeColorWarper()
        if (editor.targetedCurve) editor.targetedCurve.active = false
        const nextTool = activeSpecialTool === requestedTool
            ? toolNone : requestedTool

        if (nextTool === toolNone) {
            finishSpecialTool()
            return
        }

        if (nextTool === toolCrop) {
            editor.addCanvasNode()
            if (!editor.canvasNodeMaterialized)
                return
            editor.selectCanvasNode()
        }
        if (nextTool === toolRepair)
            editor.selectRetouchNode()
        if (nextTool === toolPaint)
            editor.paint.activate()
        if (nextTool === toolCompletion) {
            editor.selectImageCompletionNode()
            if (!editor.imageCompletionActive && !editor.beginImageCompletion())
                return
        }

        if (nextTool !== toolCompletion && editor.imageCompletionActive)
            editor.cancelImageCompletion()

        editor.setPointColorPickerActive(false)
        editor.setWhiteBalancePickerActive(false)
        editor.setRawWhiteBalancePickerActive(false)
        if (nextTool !== toolRepair)
            editor.setRetouchPickerActive(false)

        activeSpecialTool = nextTool
        editor.setMaskToolActive(nextTool === toolMask)
        editor.setCropToolActive(nextTool === toolCrop)
        if (nextTool === toolRepair)
            editor.setRetouchPickerActive(true)
    }

    function leaveSpecialTool() {
        editor.paint.cancelStroke()
        editor.paint.picking = false
        if (activeSpecialTool === toolNone) {
            editor.setRawWhiteBalancePickerActive(false)
            return
        }
        activeSpecialTool = toolNone
        editor.setMaskToolActive(false)
        editor.setCropToolActive(false)
        editor.setRetouchPickerActive(false)
        editor.setRawWhiteBalancePickerActive(false)
        if (editor.imageCompletionActive)
            editor.cancelImageCompletion()
    }

    function finishSpecialTool() {
        // The completion node owns accepted regions as well as new selections.
        // Finishing a selection must return to those editable results.
        const keepCompletion = activeSpecialTool === toolCompletion
            || editor.selectedRecipeNodeKind === "completion"
        leaveSpecialTool()
        if (!keepCompletion)
            restoreAdjustmentSelection()
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
        if (editor.hasSelectedGradeNode)
            lastAdjustmentNodeId = nextId
        if (selectionChanged)
            leaveSpecialTool()
    }

    function cancelTransientInteractionOrLeaveTool() {
        if (colorWarperExpanded) {
            closeColorWarper()
            return
        }
        if (editor.targetedCurve && editor.targetedCurve.active) {
            if (editor.targetedCurve.dragging) editor.targetedCurve.finish(true)
            else editor.targetedCurve.active = false
            return
        }
        if (editor.pointColorPickerActive) {
            editor.setPointColorPickerActive(false)
            return
        }
        if (editor.whiteBalancePickerActive) {
            editor.setWhiteBalancePickerActive(false)
            return
        }
        if (editor.rawWhiteBalancePickerActive) {
            editor.setRawWhiteBalancePickerActive(false)
            return
        }
        if (editor.retouchPickerActive) {
            editor.setRetouchPickerActive(false)
            return
        }
        finishSpecialTool()
    }

    function selectRetouchRegion(continuous, index) {
        selectedRetouchContinuous = continuous
        selectedRetouchIndex = index
    }

    Connections {
        target: precision.editor

        function onSelectedGradeNodeChanged() {
            precision.closeColorWarper()
            precision.reconcileRecipeNodeSelection()
        }

        function onSourceIdentityChanged() {
            precision.closeColorWarper()
            precision.lastAdjustmentNodeId = ""
            precision.cropAspectRatioLock = 0
            precision.selectedRetouchContinuous = true
            precision.selectedRetouchIndex = -1
            precision.leaveSpecialTool()
        }

        function onActiveChanged() {
            if (!precision.editor.active) {
                precision.closeColorWarper()
                precision.leaveSpecialTool()
            }
        }

        function onPhotoVariantsChanged() {
            const variantId = String(precision.editor.activeVariantId || "")
            if (variantId !== precision.observedVariantId) {
                precision.closeColorWarper()
                precision.observedVariantId = variantId
                precision.cropAspectRatioLock = 0
                precision.leaveSpecialTool()
            }
        }

        function onImageCompletionChanged() {
            if (precision.activeSpecialTool === precision.toolCompletion
                    && !precision.editor.imageCompletionActive
                    && !precision.editor.imageCompletionBusy) {
                precision.activeSpecialTool = precision.toolNone
            }
        }
    }

    Component.onCompleted: {
        observedRecipeNodeKind = String(editor.selectedRecipeNodeKind || "")
        observedGradeNodeId = String(editor.selectedGradeNodeId || "")
        observedGradeNodeIndex = Number(editor.selectedGradeNodeIndex)
        observedVariantId = String(editor.activeVariantId || "")
        if (editor.hasSelectedGradeNode)
            lastAdjustmentNodeId = observedGradeNodeId
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
            || precision.editor.rawWhiteBalancePickerActive
            || precision.editor.retouchPickerActive
            || precision.colorWarperExpanded
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
            objectName: "precisionGradeNodePane"
            visible: !precision.colorWarperExpanded
            Layout.preferredWidth: Math.max(220, Math.min(252, precision.width * 0.19))
            Layout.fillHeight: true
            editor: precision.editor
            interchangeController: precision.interchangeController
            lutExportController: precision.lutExportController
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
            onRepairToolRequested: {
                if (precision.activeSpecialTool !== precision.toolRepair)
                    precision.setActiveSpecialTool(precision.toolRepair)
            }
            onLiquifyToolRequested: {
                if (precision.activeSpecialTool !== precision.toolLiquify)
                    precision.setActiveSpecialTool(precision.toolLiquify)
            }
            onPaintToolRequested: {
                if (precision.activeSpecialTool !== precision.toolPaint)
                    precision.setActiveSpecialTool(precision.toolPaint)
            }
        }

        PrecisionCanvas {
            id: precisionCanvas
            objectName: "precisionCanvas"
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
            objectName: "precisionInspector"
            visible: !precision.colorWarperExpanded
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
            onColorWarperRequested: precision.openColorWarper()
            onOpenOpticsProfileLibraryRequested:
                precision.openOpticsProfileLibraryRequested()
            onToolModeRequested: mode => precision.setActiveSpecialTool(mode)
            onToolFinishRequested: precision.finishSpecialTool()
            onCropAspectRatioRequested: ratio =>
                precision.cropAspectRatioLock = ratio
            onMaskOverlayVisibilityRequested: visible =>
                precisionCanvas.maskOverlayVisible = visible
            onRetouchRegionSelectionRequested:
                (continuous, index) =>
                    precision.selectRetouchRegion(continuous, index)
        }

        PrecisionColorWarperPanel {
            id: colorWarperPanel
            visible: precision.colorWarperExpanded
            // Keep at least half the workspace for the photograph, even when
            // the editor window narrows. The hidden node pane releases space.
            Layout.preferredWidth: Math.min(precision.width * 0.5,
                Math.max(340, precision.colorWarperPanelWidth))
            Layout.maximumWidth: precision.width * 0.5
            Layout.fillHeight: true
            editor: precision.editor
            onCloseRequested: precision.closeColorWarper()
            onWidthRequested: value => precision.colorWarperPanelWidth =
                Math.max(340, Math.min(precision.width * 0.5, value))
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
