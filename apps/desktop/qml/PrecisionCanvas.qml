pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// The center precision workspace: preview transport, comparison, pixel-detail
// viewports, picker input, and display-only scopes.  Keep these mutually
// dependent interactions together; inspector controls deliberately remain out
// of this component.
//
// Parent contract (for the later behavior-neutral replacement in
// PrecisionWorkspace):
// - editor supplies the existing EditController QML facade.
// - bind the view/comparison/analysis properties below to the workspace state,
//   or mirror their generated change signals back to that state.
// - no recipe, persistent state, or image algorithm is owned here.
Rectangle {
    id: canvas
    objectName: "precisionCanvas"
    Layout.fillWidth: true
    Layout.fillHeight: true
    color: Theme.photoCanvas

    required property var editor
    required property var editPreviewPresentation
    required property var captureMetadata
    required property int activeToolMode
    required property real cropAspectRatioLock
    required property bool selectedRetouchContinuous
    required property int selectedRetouchIndex

    readonly property int toolNone: 0
    readonly property int toolMask: 1
    readonly property int toolCrop: 2
    readonly property int toolRepair: 3
    readonly property int toolLiquify: 4
    readonly property int toolCompletion: 5
    readonly property int toolPaint: 6

    // Public viewport state.
    property alias zoomFactor: viewportState.zoomFactor
    property alias fitView: viewportState.fitView
    property bool zoomToolActive: false
    property bool comparisonActive: false
    property int comparisonMode: comparisonWipeVertical
    property real comparisonPosition: 0.5
    property bool maskOverlayVisible: true
    property alias detailLoupeVisible: detailLoupeState.shown
    property alias detailLoupeFollowPointer: detailLoupeState.followPointer
    property alias detailLoupeZoomFactor: detailLoupeState.zoomFactor
    property alias detailLoupeTargetKind: detailLoupeState.targetKind
    property alias detailLoupeFocusConfirmed: detailLoupeState.focusConfirmed

    // Public display-only diagnostic state. This never mutates the edit stack.
    property bool zebraEnabled: false

    // Public comparison constants, kept here so callers do not depend on the
    // implementation's popup or surface ids.
    readonly property int comparisonWhole: 0
    readonly property int comparisonWipeVertical: 1
    readonly property int comparisonWipeHorizontal: 2
    readonly property int comparisonSideBySide: 3
    readonly property int comparisonStacked: 4

    // Public read-only display state for the outer histogram and status shell.
    readonly property bool beforeReady: editor.beforePreviewSource.length > 0
    readonly property bool displayingBefore: comparisonActive && beforeReady && comparisonMode === comparisonWhole
    readonly property var displayedHistogram: displayingBefore ? editor.beforeHistogram : editor.histogram
    readonly property string readyPreviewGeneration: readyPreviewGenerationState
    readonly property bool previewFrameReady: previewFrameReadyState
    readonly property bool beforeFrameReady: beforeFrameReadyState
    readonly property bool detailImageReady: detailImageReadyState
    readonly property bool detailImageLoadFailed: detailImageLoadFailedState
    readonly property bool showingFullDetail: !fitView && zoomFactor >= 1.0 && !comparisonActive && editor.detailMode && editor.detailTiles.length > 0 && detailImageReadyState
    readonly property bool detailLoupeAvailable: editor.active
        && activeToolMode === toolNone && !comparisonActive

    // Private transport state. The public read-only properties above keep the
    // surrounding workspace from reaching into image or Flickable ids.
    property real requestedDetailCenterX: 0.5
    property real requestedDetailCenterY: 0.5
    property bool componentReady: false
    property bool suppressViewportTracking: false
    property string readyPreviewGenerationState: ""
    property bool previewFrameReadyState: false
    property bool beforeFrameReadyState: false
    property bool detailImageReadyState: false
    property bool detailImageLoadFailedState: false

    readonly property string visiblePreviewSource: editor.previewSource.length > 0 ? editor.previewSource : editor.provisionalPreviewSource
    readonly property bool showingProvisionalPreview: editor.previewSource.length === 0 && editor.provisionalPreviewSource.length > 0
    readonly property bool dualComparison: comparisonActive && beforeReady && (comparisonMode === comparisonSideBySide || comparisonMode === comparisonStacked)
    readonly property bool scopePreviewAvailable: editor.active && previewFrameReadyState && readyPreviewGenerationState.length > 0 && !showingProvisionalPreview && !comparisonActive && Boolean(editor.histogram.valid) && !Boolean(editor.histogram.updating) && !Boolean(editor.histogram.stale) && String(editor.histogram.generation) === readyPreviewGenerationState
    readonly property real deviceScale: Math.max(1.0, Screen.devicePixelRatio)
    // Pixel-sized Recipe parameters are authored against the oriented level-zero source, not the
    // bounded 1536px preview texture. The texture remains only a compatibility fallback while the
    // first authoritative preview response is loading.
    readonly property real imagePixelWidth: editor.levelZeroWidth > 0
        ? editor.levelZeroWidth
        : (editor.detailFullWidth > 0
            ? editor.detailFullWidth
            : Math.max(1, editedPreview.sourceSize.width))
    readonly property real imagePixelHeight: editor.levelZeroHeight > 0
        ? editor.levelZeroHeight
        : (editor.detailFullHeight > 0
            ? editor.detailFullHeight
            : Math.max(1, editedPreview.sourceSize.height))
    readonly property real fitScale: viewportState.fitScale
    readonly property real displayScale: viewportState.displayScale

    readonly property color panel: Theme.panel
    readonly property color frameBorderColor: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textSecondary: Theme.textSecondary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent

    // Semantic notification points for a parent which owns the page-wide
    // workspace state. Property notify signals are also available to QML.
    signal viewStateChanged
    signal comparisonStateChanged
    signal analysisOverlayStateChanged
    signal previewFrameStateChanged
    signal detailFrameStateChanged
    signal neutralToolRequested
    signal retouchRegionSelectionRequested(bool continuous, int index)

    onZoomFactorChanged: viewStateChanged()
    onFitViewChanged: viewStateChanged()
    onComparisonActiveChanged: {
        if (comparisonActive && detailLoupeVisible)
            detailLoupeState.close()
        comparisonStateChanged()
    }
    onComparisonModeChanged: comparisonStateChanged()
    onComparisonPositionChanged: comparisonStateChanged()
    onZebraEnabledChanged: analysisOverlayStateChanged()
    onActiveToolModeChanged: {
        detailLoupeState.close()
        zoomToolActive = false;
        comparisonActive = false;
        resetView();
    }
    onPreviewFrameReadyStateChanged: previewFrameStateChanged()
    onBeforeFrameReadyStateChanged: previewFrameStateChanged()
    onReadyPreviewGenerationStateChanged: previewFrameStateChanged()
    onDetailImageReadyStateChanged: detailFrameStateChanged()
    onDetailImageLoadFailedStateChanged: detailFrameStateChanged()

    function previewGeneration(source) {
        const match = String(source).match(/[?&]generation=([^&#]+)/);
        return match && match.length > 1 ? decodeURIComponent(match[1]) : "";
    }

    function activateComparison(mode) {
        detailLoupeState.close();
        resetView();
        comparisonMode = mode;
        comparisonPosition = 0.5;
        comparisonActive = true;
        editor.requestBeforePreview();
    }

    function resetView() {
        viewportState.reset();
        detailImageReadyState = false;
        detailImageLoadFailedState = false;
        previewFlick.contentX = 0;
        previewFlick.contentY = 0;
        editor.leaveDetailMode();
        if (detailLoupeVisible)
            Qt.callLater(detailLoupeState.requestDetail);
    }

    function toggleDetailLoupe() {
        if (detailLoupeVisible)
            detailLoupeState.close()
        else if (detailLoupeAvailable) {
            resetView()
            detailLoupeState.open()
        }
    }

    function normalizedCenterX() { return viewportState.centerX }
    function normalizedCenterY() { return viewportState.centerY }

    function centerOnNormalized(nx, ny) {
        viewportState.place(nx, ny, previewFlick.width / 2, previewFlick.height / 2)
    }

    function normalizedAtViewportX(x) { return viewportState.normalizedX(x) }
    function normalizedAtViewportY(y) { return viewportState.normalizedY(y) }

    function placeNormalizedAtViewport(nx, ny, x, y) {
        viewportState.place(nx, ny, x, y)
    }

    function zoomAnchorX(x) {
        if (!dualComparison)
            return x
        const paneWidth = comparisonMode === comparisonSideBySide
            ? previewFlick.width / 2 : previewFlick.width
        return x % paneWidth - 10
            + (previewFlick.width - viewportState.viewportWidth) / 2
    }

    function zoomAnchorY(y) {
        if (!dualComparison)
            return y
        const paneHeight = comparisonMode === comparisonStacked
            ? previewFlick.height / 2 : previewFlick.height
        return y % paneHeight - 10
            + (previewFlick.height - viewportState.viewportHeight) / 2
    }

    function requestVisibleDetail() {
        if (viewportState.continuousZoomActive)
            return;
        if (detailLoupeVisible && fitView && !comparisonActive
                && editor.active) {
            detailLoupeState.requestDetail()
            return
        }
        if (fitView || zoomFactor < 1.0 || comparisonActive || !editor.active) {
            editor.leaveDetailMode();
            return;
        }
        requestedDetailCenterX = normalizedCenterX();
        requestedDetailCenterY = normalizedCenterY();
        const pixelWidth = Math.max(1, Math.ceil(previewFlick.width / displayScale));
        const pixelHeight = Math.max(1, Math.ceil(previewFlick.height / displayScale));
        if (pixelWidth > 8192 || pixelHeight > 8192) {
            editor.leaveDetailMode();
            detailImageLoadFailedState = true;
            return;
        }
        detailImageLoadFailedState = false;
        editor.requestDetailViewport(requestedDetailCenterX, requestedDetailCenterY, pixelWidth, pixelHeight, true);
    }

    function directViewportPositionChanged() {
        if (!componentReady || fitView || zoomFactor < 1.0 || comparisonActive || !editor.active || previewFlick.moving || previewFlick.flicking || suppressViewportTracking || viewportState.applyingTransform || viewportState.continuousZoomActive || (!editor.detailMode && !detailImageReadyState))
            return;
        directViewportSettle.restart();
    }

    function setPixelZoom(value) {
        detailLoupeState.close()
        viewportState.zoomAt(previewFlick.width / 2, previewFlick.height / 2, value);
        canvas.requestVisibleDetail();
    }

    function zoomAtViewport(viewportX, viewportY, value, settleDetail) {
        detailLoupeState.close()
        viewportState.zoomAt(zoomAnchorX(viewportX), zoomAnchorY(viewportY), value);
        if (settleDetail)
            canvas.requestVisibleDetail();
    }

    function zoomStepAtViewport(viewportX, viewportY, direction) {
        if (direction > 0 && fitView) {
            zoomAtViewport(viewportX, viewportY, 1.0, true);
            return;
        }
        const steps = [0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0];
        if (direction < 0 && zoomFactor <= steps[0] + 0.001) {
            resetView();
            return;
        }
        if (direction > 0) {
            for (let index = 0; index < steps.length; ++index) {
                if (steps[index] > zoomFactor + 0.001) {
                    zoomAtViewport(viewportX, viewportY, steps[index], true);
                    return;
                }
            }
            return;
        }
        for (let index = steps.length - 1; index >= 0; --index) {
            if (steps[index] < zoomFactor - 0.001) {
                zoomAtViewport(viewportX, viewportY, steps[index], true);
                return;
            }
        }
    }

    function beginContinuousZoom() {
        detailLoupeState.close()
        directViewportSettle.stop();
        viewportState.beginContinuousZoom();
    }

    function finishContinuousZoom() {
        viewportState.finishContinuousZoom();
        canvas.requestVisibleDetail();
    }

    Connections {
        target: canvas.editor
        function onSourcePathChanged() {
            canvas.comparisonActive = false;
            canvas.previewFrameReadyState = false;
            canvas.beforeFrameReadyState = false;
            canvas.readyPreviewGenerationState = "";
            canvas.resetView();
        }

    }

    onDeviceScaleChanged: {
        if (detailLoupeVisible && fitView)
            return
        if (!componentReady || fitView || zoomFactor < 1.0 || !editor.active)
            return;
        editor.leaveDetailMode();
        Qt.callLater(function () {
            canvas.centerOnNormalized(canvas.requestedDetailCenterX, canvas.requestedDetailCenterY);
            canvas.requestVisibleDetail();
        });
    }

    Component.onCompleted: componentReady = true

    Timer {
        id: directViewportSettle
        interval: 70
        repeat: false
        onTriggered: canvas.requestVisibleDetail()
    }

    PrecisionViewportState {
        id: viewportState
        flickable: previewFlick
        imagePixelWidth: canvas.imagePixelWidth
        imagePixelHeight: canvas.imagePixelHeight
        deviceScale: canvas.deviceScale
        viewportWidth: canvas.dualComparison
            ? Math.max(1, previewFlick.width
                / (canvas.comparisonMode === canvas.comparisonSideBySide ? 2 : 1) - 20)
            : previewFlick.width
        viewportHeight: canvas.dualComparison
            ? Math.max(1, previewFlick.height
                / (canvas.comparisonMode === canvas.comparisonStacked ? 2 : 1) - 20)
            : previewFlick.height
    }

    PrecisionDetailLoupeState {
        id: detailLoupeState
        editor: canvas.editor
        captureMetadata: canvas.captureMetadata
        requestAvailable: canvas.detailLoupeAvailable && canvas.fitView
        deviceScale: canvas.deviceScale
        viewportWidth: detailLoupe.detailViewportWidth
        viewportHeight: detailLoupe.detailViewportHeight
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PrecisionCanvasToolbar {
            id: canvasToolbar
            editor: canvas.editor
            zebraEnabled: canvas.zebraEnabled
            comparisonActive: canvas.comparisonActive
            comparisonMode: canvas.comparisonMode
            comparisonWhole: canvas.comparisonWhole
            comparisonWipeVertical: canvas.comparisonWipeVertical
            comparisonWipeHorizontal: canvas.comparisonWipeHorizontal
            comparisonSideBySide: canvas.comparisonSideBySide
            comparisonStacked: canvas.comparisonStacked
            fitView: canvas.fitView
            zoomFactor: canvas.zoomFactor
            zoomToolActive: canvas.zoomToolActive
            detailLoupeVisible: canvas.detailLoupeVisible
            detailLoupeAvailable: canvas.detailLoupeAvailable
            onZebraToggleRequested: canvas.zebraEnabled = !canvas.zebraEnabled
            onComparisonDisableRequested: canvas.comparisonActive = false
            onComparisonModeRequested: mode => canvas.activateComparison(mode)
            onZoomRequested: value => canvas.setPixelZoom(value)
            onZoomToolToggleRequested: {
                canvas.neutralToolRequested();
                canvas.zoomToolActive = !canvas.zoomToolActive;
            }
            onFitRequested: canvas.resetView()
            onDetailLoupeToggleRequested: canvas.toggleDetailLoupe()
        }

        Flickable {
            id: previewFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentWidth: viewportState.contentWidth
            contentHeight: viewportState.contentHeight
            interactive: !viewportState.continuousZoomActive && !canvas.zoomToolActive && (contentWidth > width || contentHeight > height)
            onMovementStarted: {
                directViewportSettle.stop();
            }
            onMovementEnded: canvas.requestVisibleDetail()
            onContentXChanged: canvas.directViewportPositionChanged()
            onContentYChanged: canvas.directViewportPositionChanged()
            onWidthChanged: {
                if (canvas.editor.detailMode)
                    canvas.requestVisibleDetail();
            }
            onHeightChanged: {
                if (canvas.editor.detailMode)
                    canvas.requestVisibleDetail();
            }

            Item {
                id: photoSurface
                x: viewportState.imageX
                y: viewportState.imageY
                width: viewportState.imageWidth
                height: viewportState.imageHeight

                Image {
                    id: editedPreview
                    anchors.fill: parent
                    source: liveEditedPreview.fallbackSource
                    fillMode: Image.Stretch
                    asynchronous: true
                    cache: false
                    visible: !canvas.dualComparison
                    // Keep the last decoded texture on screen until the
                    // replacement generation is actually ready. Without
                    // this, every slider update briefly exposes the
                    // canvas while Qt decodes the next JPEG.
                    retainWhileLoading: true
                    smooth: true
                    onSourceChanged: {
                        canvas.previewFrameReadyState = false;
                        canvas.readyPreviewGenerationState = "";
                    }
                    onStatusChanged: {
                        if (status === Image.Ready) {
                            canvas.previewFrameReadyState = true;
                            canvas.readyPreviewGenerationState = canvas.previewGeneration(source);
                        } else if (status === Image.Null) {
                            canvas.previewFrameReadyState = false;
                            canvas.readyPreviewGenerationState = "";
                        } else if (status === Image.Error) {
                            canvas.readyPreviewGenerationState = "";
                        }
                    }
                }

                EditPreviewTextureItem {
                    id: liveEditedPreview
                    objectName: "liveEditedPreview"
                    anchors.fill: parent
                    z: 1
                    presentationRegistry: canvas.editPreviewPresentation
                    source: canvas.visiblePreviewSource
                    liveAdmissionEnabled: !canvas.dualComparison
                    fillMode: EditPreviewTextureItem.Stretch
                    visible: !canvas.dualComparison
                    onSourceChanged: {
                        canvas.previewFrameReadyState = false;
                        canvas.readyPreviewGenerationState = "";
                    }
                    onPresentedGenerationChanged: {
                        if (presentedGeneration.length > 0) {
                            canvas.previewFrameReadyState = true;
                            canvas.readyPreviewGenerationState = presentedGeneration;
                        } else {
                            canvas.previewFrameReadyState = false;
                            canvas.readyPreviewGenerationState = "";
                        }
                    }
                }

                Image {
                    id: displayZebraOverlay
                    anchors.fill: parent
                    source: canvas.scopePreviewAvailable ? "image://shadow-edit/scope/zebra/current?generation=" + canvas.readyPreviewGeneration : ""
                    fillMode: Image.Stretch
                    asynchronous: true
                    cache: false
                    retainWhileLoading: true
                    smooth: false
                    mipmap: false
                    visible: canvas.zebraEnabled && canvas.scopePreviewAvailable && status === Image.Ready
                    z: 10
                }

                PrecisionComparisonSurface {
                    id: comparisonSurface
                    parent: canvas.dualComparison ? previewFlick : photoSurface
                    anchors.fill: parent
                    imageDisplayWidth: viewportState.imageWidth
                    imageDisplayHeight: viewportState.imageHeight
                    imageCenterX: viewportState.centerX
                    imageCenterY: viewportState.centerY
                    z: 20
                    editor: canvas.editor
                    editPreviewPresentation: canvas.editPreviewPresentation
                    comparisonActive: canvas.comparisonActive
                    comparisonMode: canvas.comparisonMode
                    comparisonPosition: canvas.comparisonPosition
                    beforeReady: canvas.beforeReady
                    afterPreviewSource: canvas.visiblePreviewSource
                    wipeVerticalMode: canvas.comparisonWipeVertical
                    wipeHorizontalMode: canvas.comparisonWipeHorizontal
                    sideBySideMode: canvas.comparisonSideBySide
                    stackedMode: canvas.comparisonStacked
                    onBeforeFrameReadyChanged: canvas.beforeFrameReadyState = comparisonSurface.beforeFrameReady
                    onComparisonPositionRequested: nextPosition => canvas.comparisonPosition = nextPosition
                }

                PrecisionDetailSurface {
                    id: detailSurface
                    z: 2
                    tile: canvas.editor.detailTiles.length > 0
                        ? canvas.editor.detailTiles[0] : ({})
                    displayScale: canvas.displayScale
                    visible: canvas.showingFullDetail
                    onReadyChanged: canvas.detailImageReadyState = ready
                    onLoadFailedChanged: canvas.detailImageLoadFailedState = loadFailed
                }

                PrecisionMaskCoverageOverlay {
                    id: maskCoverageOverlay
                    anchors.fill: parent
                    z: 94
                    editor: canvas.editor
                    readyPreviewGeneration: canvas.readyPreviewGeneration
                    coverageVisible: canvas.maskOverlayVisible
                    interactionEnabled: canvas.activeToolMode === canvas.toolMask && !canvas.comparisonActive && canvas.previewFrameReady
                }

                PrecisionLocalMaskOverlay {
                    anchors.fill: parent
                    z: 95
                    editor: canvas.editor
                    nativeCoverageReady: maskCoverageOverlay.coverageReady
                    coverageVisible: canvas.maskOverlayVisible
                    interactionEnabled: canvas.activeToolMode === canvas.toolMask && !canvas.comparisonActive && !canvas.editor.aiMaskPromptActive && !canvas.editor.pointColorPickerActive && !canvas.editor.whiteBalancePickerActive && !canvas.editor.retouchPickerActive
                }

                Image {
                    objectName: "subjectEmphasisCandidateOverlay"
                    anchors.fill: parent
                    z: 100
                    source: canvas.editor.subjectEmphasis && canvas.editor.subjectEmphasis.active
                        ? canvas.editor.subjectEmphasis.maskSource : ""
                    visible: !canvas.comparisonActive && canvas.previewFrameReady
                        && source.toString().length > 0
                    fillMode: Image.Stretch
                    cache: false
                }

                PrecisionAiMaskPromptOverlay {
                    anchors.fill: parent
                    z: 101
                    interactionEnabled: canvas.editor.aiMaskPromptActive && !canvas.comparisonActive && canvas.previewFrameReady
                    busy: canvas.editor.aiMaskBusy
                    faceRegionMode: canvas.editor.aiMaskFaceRegionMode
                    semanticMode: canvas.editor.aiMaskSemanticMode
                    foregroundMode: canvas.editor.aiMaskForegroundMode
                    promptPoints: canvas.editor.aiMaskPromptPoints
                    peopleCount: canvas.editor.aiMaskPeople.length
                    candidateSource: canvas.editor.aiMaskCandidateSource
                    candidateVisible: canvas.editor.aiMaskHasCandidate
                    foregroundColor: Theme.labelGreen
                    backgroundColor: Theme.labelRed
                    candidateColor: Theme.labelPurple
                    onPointRequested: (normalizedX, normalizedY, foreground) => canvas.editor.addAiMaskPromptPoint(normalizedX, normalizedY, foreground)
                    onUndoRequested: canvas.editor.undoAiMaskPromptPoint()
                    onClearRequested: canvas.editor.clearAiMaskPromptPoints()
                    onForegroundModeRequested: foreground => canvas.editor.aiMaskForegroundMode = foreground
                    onRetryRequested: canvas.editor.generateAiMask()
                    onApplyRequested: canvas.editor.applyAiMaskCandidate()
                    onCancelRequested: canvas.editor.cancelAiMaskPrompt()
                }

                PrecisionAiCompletionOverlay {
                    anchors.fill: parent
                    z: 102
                    editor: canvas.editor
                    interactionEnabled:
                        canvas.activeToolMode === canvas.toolCompletion
                        && !canvas.comparisonActive
                        && canvas.previewFrameReady
                }

                PrecisionCropOverlay {
                    anchors.fill: parent
                    z: 97
                    editor: canvas.editor
                    aspectRatioLock: canvas.cropAspectRatioLock
                    interactionEnabled: canvas.activeToolMode === canvas.toolCrop && !canvas.comparisonActive
                }

                PrecisionRetouchOverlay {
                    anchors.fill: parent
                    z: 103
                    editor: canvas.editor
                    interactionEnabled: canvas.activeToolMode === canvas.toolRepair && !canvas.comparisonActive
                    selectedContinuous: canvas.selectedRetouchContinuous
                    selectedIndex: canvas.selectedRetouchIndex
                    levelZeroWidth: canvas.imagePixelWidth
                    levelZeroHeight: canvas.imagePixelHeight
                    onRegionSelected: (continuous, index) => canvas.retouchRegionSelectionRequested(continuous, index)
                }

                PrecisionPaintOverlay {
                    anchors.fill: parent
                    z: 105
                    editor: canvas.editor
                    previewReady: canvas.previewFrameReady
                    previewGeneration: canvas.readyPreviewGeneration
                    outputAspectRatio: canvas.imagePixelWidth / Math.max(1, canvas.imagePixelHeight)
                    interactionEnabled: canvas.activeToolMode === canvas.toolPaint && !canvas.comparisonActive
                }

                PrecisionLiquifyOverlay {
                    anchors.fill: parent
                    z: 104
                    editor: canvas.editor
                    previewItem: liveEditedPreview
                    previewReady: canvas.previewFrameReady
                    previewGeneration: canvas.readyPreviewGeneration
                    outputAspectRatio:
                        canvas.imagePixelWidth
                        / Math.max(1, canvas.imagePixelHeight)
                    interactionEnabled:
                        canvas.activeToolMode === canvas.toolLiquify
                        && !canvas.comparisonActive
                        && (!canvas.editor.liquifyNodeMaterialized
                            || canvas.editor.liquifyNodeEnabled)
                }

                PrecisionCanvasPickerInput {
                    anchors.fill: parent
                    z: 100
                    editor: canvas.editor
                    previewItem: photoSurface
                    samplePreviewItem: liveEditedPreview
                    previewContentRect: Qt.rect(0, 0, photoSurface.width, photoSurface.height)
                    previewFrameReady: canvas.previewFrameReady
                    readyPreviewGeneration: canvas.readyPreviewGeneration
                    displayScale: canvas.displayScale
                    levelZeroWidth: canvas.imagePixelWidth
                    levelZeroHeight: canvas.imagePixelHeight
                    selectedRetouchIndex: canvas.selectedRetouchIndex
                    interactionEnabled: !canvas.comparisonActive && (canvas.editor.retouchPickerActive || ((canvas.editor.pointColorPickerActive || canvas.editor.whiteBalancePickerActive || canvas.editor.rawWhiteBalancePickerActive) && canvas.previewFrameReady && canvas.readyPreviewGeneration.length > 0))
                }

                HoverHandler {
                    enabled: canvas.detailLoupeVisible
                        && canvas.detailLoupeFollowPointer
                        && canvas.detailLoupeAvailable
                    cursorShape: Qt.CrossCursor
                    onPointChanged: detailLoupeState.setTarget(
                        point.position.x / Math.max(1, photoSurface.width),
                        point.position.y / Math.max(1, photoSurface.height))
                }

                TapHandler {
                    enabled: canvas.detailLoupeVisible
                        && !canvas.detailLoupeFollowPointer
                        && canvas.detailLoupeAvailable
                    acceptedButtons: Qt.LeftButton
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: eventPoint => detailLoupeState.setTarget(
                        eventPoint.position.x / Math.max(1, photoSurface.width),
                        eventPoint.position.y / Math.max(1, photoSurface.height))
                }
            }

            ScrollBar.horizontal: ScrollBar {
                policy: ScrollBar.AsNeeded
                onPressedChanged: {
                    if (pressed) {
                        directViewportSettle.stop();
                    } else {
                        Qt.callLater(canvas.requestVisibleDetail);
                    }
                }
            }
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                onPressedChanged: {
                    if (pressed) {
                        directViewportSettle.stop();
                    } else {
                        Qt.callLater(canvas.requestVisibleDetail);
                    }
                }
            }
        }
    }

    PrecisionCanvasZoomInput {
        parent: previewFlick
        anchors.fill: parent
        z: 150
        interactionEnabled: canvas.editor.active
            && canvas.activeToolMode === canvas.toolNone
            && !canvas.detailLoupeVisible
        toolActive: canvas.zoomToolActive
        fitView: canvas.fitView
        zoomFactor: canvas.zoomFactor
        fitZoomFactor: canvas.fitScale * canvas.deviceScale
        onZoomStepRequested: (viewportX, viewportY, direction) => canvas.zoomStepAtViewport(viewportX, viewportY, direction)
        onContinuousZoomStarted: canvas.beginContinuousZoom()
        onContinuousZoomRequested: (viewportX, viewportY, requestedZoom) => canvas.zoomAtViewport(viewportX, viewportY, requestedZoom, false)
        onContinuousZoomFinished: canvas.finishContinuousZoom()
    }

    PrecisionCanvasStatusOverlays {
        anchors.fill: parent
        z: 200
        editor: canvas.editor
        comparisonActive: canvas.comparisonActive
        comparisonMode: canvas.comparisonMode
        showingFullDetail: canvas.showingFullDetail
        fitView: canvas.fitView
        zoomFactor: canvas.zoomFactor
        detailImageReady: canvas.detailImageReady
        detailImageLoadFailed: canvas.detailImageLoadFailed
        beforeReady: canvas.beforeReady
        beforeFrameReady: canvas.beforeFrameReady
        previewFrameReady: canvas.previewFrameReady
        previewLoadFailed: editedPreview.status === Image.Error
        comparisonWhole: canvas.comparisonWhole
        comparisonWipeVertical: canvas.comparisonWipeVertical
        comparisonWipeHorizontal: canvas.comparisonWipeHorizontal
        comparisonSideBySide: canvas.comparisonSideBySide
    }

    PrecisionDetailLoupe {
        id: detailLoupe
        z: 240
        visible: canvas.detailLoupeVisible
        enabled: visible
        editor: canvas.editor
        followPointer: canvas.detailLoupeFollowPointer
        zoomFactor: canvas.detailLoupeZoomFactor
        targetCenterX: detailLoupeState.centerX
        targetCenterY: detailLoupeState.centerY
        deviceScale: canvas.deviceScale
        dragTopBoundary: canvasToolbar.height + 8
        targetKind: canvas.detailLoupeTargetKind
        focusConfirmed: canvas.detailLoupeFocusConfirmed
        onCloseRequested: detailLoupeState.close()
        onFollowPointerToggleRequested:
            detailLoupeState.toggleFollowPointer()
        onZoomFactorRequested: value =>
            detailLoupeState.setZoomFactor(value)
    }
}
