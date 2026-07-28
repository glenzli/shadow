pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

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
    Layout.fillWidth: true
    Layout.fillHeight: true
    color: Theme.photoCanvas

    required property var editor
    required property int activeToolMode
    required property real cropAspectRatioLock

    readonly property int toolNone: 0
    readonly property int toolMask: 1
    readonly property int toolCrop: 2
    readonly property int toolRepair: 3

    // Public viewport state.
    property real zoomFactor: 1.0
    property bool fitView: true
    property bool zoomToolActive: false
    property bool comparisonActive: false
    property int comparisonMode: comparisonWipeVertical
    property real comparisonPosition: 0.5

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
    readonly property bool displayingBefore: comparisonActive && beforeReady
        && comparisonMode === comparisonWhole
    readonly property var displayedHistogram: displayingBefore
        ? editor.beforeHistogram : editor.histogram
    readonly property string readyPreviewGeneration: readyPreviewGenerationState
    readonly property bool previewFrameReady: previewFrameReadyState
    readonly property bool beforeFrameReady: beforeFrameReadyState
    readonly property bool detailImageReady: detailImageReadyState
    readonly property bool detailImageLoadFailed: detailImageLoadFailedState
    readonly property bool showingFullDetail: !fitView && zoomFactor >= 1.0
        && !comparisonActive && editor.detailMode && editor.detailTiles.length > 0
        && detailImageReadyState

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

    readonly property string visiblePreviewSource: editor.previewSource.length > 0
        ? editor.previewSource : editor.provisionalPreviewSource
    readonly property bool showingProvisionalPreview: editor.previewSource.length === 0
        && editor.provisionalPreviewSource.length > 0
    readonly property bool dualComparison: comparisonActive && beforeReady
        && (comparisonMode === comparisonSideBySide
            || comparisonMode === comparisonStacked)
    readonly property bool scopePreviewAvailable: editor.active
        && previewFrameReadyState
        && readyPreviewGenerationState.length > 0
        && !showingProvisionalPreview
        && !comparisonActive
    readonly property real deviceScale: Math.max(1.0, Screen.devicePixelRatio)
    readonly property real imagePixelWidth: editor.detailFullWidth > 0
        ? editor.detailFullWidth
        : Math.max(1, editedPreview.sourceSize.width)
    readonly property real imagePixelHeight: editor.detailFullHeight > 0
        ? editor.detailFullHeight
        : Math.max(1, editedPreview.sourceSize.height)
    readonly property real fitScale: Math.min(
        previewFlick.width / imagePixelWidth,
        previewFlick.height / imagePixelHeight
    )
    readonly property real displayScale: fitView
        ? Math.max(0.0001, fitScale)
        : zoomFactor / deviceScale

    readonly property color panel: Theme.panel
    readonly property color frameBorderColor: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textSecondary: Theme.textSecondary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent

    // Semantic notification points for a parent which owns the page-wide
    // workspace state. Property notify signals are also available to QML.
    signal viewStateChanged()
    signal comparisonStateChanged()
    signal analysisOverlayStateChanged()
    signal previewFrameStateChanged()
    signal detailFrameStateChanged()
    signal neutralToolRequested()

    onZoomFactorChanged: viewStateChanged()
    onFitViewChanged: viewStateChanged()
    onComparisonActiveChanged: comparisonStateChanged()
    onComparisonModeChanged: comparisonStateChanged()
    onComparisonPositionChanged: comparisonStateChanged()
    onZebraEnabledChanged: analysisOverlayStateChanged()
    onActiveToolModeChanged: {
        zoomToolActive = false
        comparisonActive = false
        resetView()
    }
    onPreviewFrameReadyStateChanged: previewFrameStateChanged()
    onBeforeFrameReadyStateChanged: previewFrameStateChanged()
    onReadyPreviewGenerationStateChanged: previewFrameStateChanged()
    onDetailImageReadyStateChanged: detailFrameStateChanged()
    onDetailImageLoadFailedStateChanged: detailFrameStateChanged()

    function previewGeneration(source) {
        const match = String(source).match(/[?&]generation=([^&#]+)/)
        return match && match.length > 1 ? decodeURIComponent(match[1]) : ""
    }

    function activateComparison(mode) {
        resetView()
        comparisonMode = mode
        comparisonPosition = 0.5
        comparisonActive = true
        editor.requestBeforePreview()
    }

    function resetView() {
        fitView = true
        zoomFactor = 1.0
        detailImageReadyState = false
        detailImageLoadFailedState = false
        previewFlick.contentX = 0
        previewFlick.contentY = 0
        editor.leaveDetailMode()
    }

    function normalizedCenterX() {
        if (photoSurface.width <= 0)
            return 0.5
        return Math.max(0, Math.min(1,
            (previewFlick.contentX + previewFlick.width / 2 - photoSurface.x)
                / photoSurface.width))
    }

    function normalizedCenterY() {
        if (photoSurface.height <= 0)
            return 0.5
        return Math.max(0, Math.min(1,
            (previewFlick.contentY + previewFlick.height / 2 - photoSurface.y)
                / photoSurface.height))
    }

    function centerOnNormalized(nx, ny) {
        previewFlick.contentX = Math.max(0, Math.min(
            previewFlick.contentWidth - previewFlick.width,
            photoSurface.x + nx * photoSurface.width - previewFlick.width / 2
        ))
        previewFlick.contentY = Math.max(0, Math.min(
            previewFlick.contentHeight - previewFlick.height,
            photoSurface.y + ny * photoSurface.height - previewFlick.height / 2
        ))
    }

    function normalizedAtViewportX(viewportX) {
        if (photoSurface.width <= 0)
            return 0.5
        return Math.max(0, Math.min(1,
            (previewFlick.contentX + viewportX - photoSurface.x)
                / photoSurface.width))
    }

    function normalizedAtViewportY(viewportY) {
        if (photoSurface.height <= 0)
            return 0.5
        return Math.max(0, Math.min(1,
            (previewFlick.contentY + viewportY - photoSurface.y)
                / photoSurface.height))
    }

    function placeNormalizedAtViewport(nx, ny, viewportX, viewportY) {
        previewFlick.contentX = Math.max(0, Math.min(
            previewFlick.contentWidth - previewFlick.width,
            photoSurface.x + nx * photoSurface.width - viewportX
        ))
        previewFlick.contentY = Math.max(0, Math.min(
            previewFlick.contentHeight - previewFlick.height,
            photoSurface.y + ny * photoSurface.height - viewportY
        ))
    }

    function requestVisibleDetail() {
        if (fitView || zoomFactor < 1.0 || comparisonActive || !editor.active) {
            editor.leaveDetailMode()
            return
        }
        requestedDetailCenterX = normalizedCenterX()
        requestedDetailCenterY = normalizedCenterY()
        const pixelWidth = Math.max(1,
            Math.ceil(previewFlick.width / displayScale))
        const pixelHeight = Math.max(1,
            Math.ceil(previewFlick.height / displayScale))
        if (pixelWidth > 8192 || pixelHeight > 8192) {
            editor.leaveDetailMode()
            detailImageLoadFailedState = true
            return
        }
        detailImageLoadFailedState = false
        editor.requestDetailViewport(
            requestedDetailCenterX,
            requestedDetailCenterY,
            pixelWidth,
            pixelHeight
        )
    }

    function directViewportPositionChanged() {
        if (!componentReady || fitView || zoomFactor < 1.0 || comparisonActive
                || !editor.active || previewFlick.moving || previewFlick.flicking
                || suppressViewportTracking
                || (!editor.detailMode && !detailImageReadyState))
            return
        directViewportSettle.restart()
    }

    function setPixelZoom(value) {
        zoomAtViewport(
            previewFlick.width / 2,
            previewFlick.height / 2,
            value,
            true)
    }

    function zoomAtViewport(viewportX, viewportY, value, settleDetail) {
        const anchorX = normalizedAtViewportX(viewportX)
        const anchorY = normalizedAtViewportY(viewportY)
        fitView = false
        comparisonActive = false
        zoomFactor = Math.max(0.05, Math.min(4.0, value))
        Qt.callLater(function() {
            canvas.placeNormalizedAtViewport(
                anchorX, anchorY, viewportX, viewportY)
            if (settleDetail)
                canvas.requestVisibleDetail()
        })
    }

    function zoomStepAtViewport(viewportX, viewportY, direction) {
        if (direction > 0 && fitView) {
            zoomAtViewport(viewportX, viewportY, 1.0, true)
            return
        }
        const steps = [0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0]
        if (direction < 0 && zoomFactor <= steps[0] + 0.001) {
            resetView()
            return
        }
        if (direction > 0) {
            for (let index = 0; index < steps.length; ++index) {
                if (steps[index] > zoomFactor + 0.001) {
                    zoomAtViewport(
                        viewportX, viewportY, steps[index], true)
                    return
                }
            }
            return
        }
        for (let index = steps.length - 1; index >= 0; --index) {
            if (steps[index] < zoomFactor - 0.001) {
                zoomAtViewport(viewportX, viewportY, steps[index], true)
                return
            }
        }
    }

    function beginContinuousZoom() {
        directViewportSettle.stop()
        detailImageReadyState = false
        detailImageLoadFailedState = false
        editor.leaveDetailMode()
    }

    function finishContinuousZoom() {
        Qt.callLater(canvas.requestVisibleDetail)
    }

    Connections {
        target: canvas.editor
        function onSourcePathChanged() {
            canvas.comparisonActive = false
            canvas.previewFrameReadyState = false
            canvas.beforeFrameReadyState = false
            canvas.readyPreviewGenerationState = ""
            canvas.resetView()
        }
        function onDetailGeometryChanged() {
            if (!canvas.fitView && canvas.editor.detailMode) {
                Qt.callLater(function() {
                    canvas.suppressViewportTracking = true
                    canvas.centerOnNormalized(
                        canvas.requestedDetailCenterX,
                        canvas.requestedDetailCenterY
                    )
                    canvas.suppressViewportTracking = false
                })
            }
        }
        function onDetailTilesChanged() {
            canvas.detailImageReadyState = false
            canvas.detailImageLoadFailedState = false
        }
    }

    onDeviceScaleChanged: {
        if (!componentReady || fitView || zoomFactor < 1.0 || !editor.active)
            return
        editor.leaveDetailMode()
        Qt.callLater(function() {
            canvas.centerOnNormalized(
                canvas.requestedDetailCenterX,
                canvas.requestedDetailCenterY
            )
            canvas.requestVisibleDetail()
        })
    }

    Component.onCompleted: componentReady = true

    Timer {
        id: directViewportSettle
        interval: 70
        repeat: false
        onTriggered: canvas.requestVisibleDetail()
    }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                PrecisionCanvasToolbar {
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
                    onZebraToggleRequested:
                        canvas.zebraEnabled = !canvas.zebraEnabled
                    onComparisonDisableRequested:
                        canvas.comparisonActive = false
                    onComparisonModeRequested: mode =>
                        canvas.activateComparison(mode)
                    onZoomRequested: value => canvas.setPixelZoom(value)
                    onZoomToolToggleRequested: {
                        canvas.neutralToolRequested()
                        canvas.zoomToolActive = !canvas.zoomToolActive
                    }
                    onFitRequested: canvas.resetView()
                }

                Flickable {
                    id: previewFlick
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    contentWidth: Math.max(width, photoSurface.width)
                    contentHeight: Math.max(height, photoSurface.height)
                    interactive: !canvas.zoomToolActive
                        && (contentWidth > width || contentHeight > height)
                    onMovementStarted: {
                        directViewportSettle.stop()
                    }
                    onMovementEnded: canvas.requestVisibleDetail()
                    onContentXChanged: canvas.directViewportPositionChanged()
                    onContentYChanged: canvas.directViewportPositionChanged()
                    onWidthChanged: {
                        if (canvas.editor.detailMode)
                            canvas.requestVisibleDetail()
                    }
                    onHeightChanged: {
                        if (canvas.editor.detailMode)
                            canvas.requestVisibleDetail()
                    }

                    Item {
                        id: photoSurface
                        x: (previewFlick.contentWidth - width) / 2
                        y: (previewFlick.contentHeight - height) / 2
                        width: canvas.dualComparison
                            ? previewFlick.width
                            : canvas.imagePixelWidth * canvas.displayScale
                        height: canvas.dualComparison
                            ? previewFlick.height
                            : canvas.imagePixelHeight * canvas.displayScale

                        Image {
                            id: editedPreview
                            anchors.fill: parent
                            source: canvas.visiblePreviewSource
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
                                canvas.previewFrameReadyState = false
                                canvas.readyPreviewGenerationState = ""
                            }
                            onStatusChanged: {
                                if (status === Image.Ready) {
                                    canvas.previewFrameReadyState = true
                                    canvas.readyPreviewGenerationState
                                        = canvas.previewGeneration(source)
                                } else if (status === Image.Null) {
                                    canvas.previewFrameReadyState = false
                                    canvas.readyPreviewGenerationState = ""
                                } else if (status === Image.Error) {
                                    canvas.readyPreviewGenerationState = ""
                                }
                            }
                        }

                        Image {
                            id: displayZebraOverlay
                            anchors.fill: parent
                            source: canvas.scopePreviewAvailable
                                ? "image://shadow-edit/scope/zebra/current?generation="
                                    + canvas.readyPreviewGeneration : ""
                            fillMode: Image.Stretch
                            asynchronous: true
                            cache: false
                            retainWhileLoading: true
                            smooth: false
                            mipmap: false
                            visible: canvas.zebraEnabled
                                && canvas.scopePreviewAvailable
                                && status === Image.Ready
                            z: 10
                        }

                        PrecisionComparisonSurface {
                            id: comparisonSurface
                            anchors.fill: parent
                            z: 20
                            editor: canvas.editor
                            comparisonActive: canvas.comparisonActive
                            comparisonMode: canvas.comparisonMode
                            comparisonPosition: canvas.comparisonPosition
                            beforeReady: canvas.beforeReady
                            afterPreviewSource: canvas.visiblePreviewSource
                            wipeVerticalMode: canvas.comparisonWipeVertical
                            wipeHorizontalMode: canvas.comparisonWipeHorizontal
                            sideBySideMode: canvas.comparisonSideBySide
                            stackedMode: canvas.comparisonStacked
                            onBeforeFrameReadyChanged:
                                canvas.beforeFrameReadyState
                                    = comparisonSurface.beforeFrameReady
                            onComparisonPositionRequested: nextPosition =>
                                canvas.comparisonPosition = nextPosition
                        }

                        Repeater {
                            model: canvas.editor.detailTiles
                            delegate: Image {
                                required property var modelData
                                x: modelData.x * canvas.displayScale
                                y: modelData.y * canvas.displayScale
                                width: modelData.width * canvas.displayScale
                                height: modelData.height * canvas.displayScale
                                source: modelData.source
                                fillMode: Image.Stretch
                                asynchronous: true
                                cache: false
                                smooth: false
                                visible: canvas.showingFullDetail
                                onStatusChanged: {
                                    if (status === Image.Ready)
                                        canvas.detailImageReadyState = true
                                    else if (status === Image.Error) {
                                        canvas.detailImageReadyState = false
                                        canvas.detailImageLoadFailedState = true
                                    }
                                }
                            }
                        }

                        PrecisionLocalMaskOverlay {
                            anchors.fill: parent
                            z: 95
                            editor: canvas.editor
                            interactionEnabled: canvas.activeToolMode
                                    === canvas.toolMask
                                && !canvas.comparisonActive
                                && !canvas.editor.pointColorPickerActive
                                && !canvas.editor.whiteBalancePickerActive
                                && !canvas.editor.retouchPickerActive
                        }

                        PrecisionCropOverlay {
                            anchors.fill: parent
                            z: 97
                            editor: canvas.editor
                            aspectRatioLock: canvas.cropAspectRatioLock
                            interactionEnabled: canvas.activeToolMode
                                === canvas.toolCrop
                                && !canvas.comparisonActive
                                && canvas.previewFrameReady
                        }

                        PrecisionRetouchOverlay {
                            anchors.fill: parent
                            z: 103
                            editor: canvas.editor
                            interactionEnabled: canvas.activeToolMode
                                === canvas.toolRepair
                                && !canvas.comparisonActive
                                && canvas.previewFrameReady
                            levelZeroWidth: canvas.imagePixelWidth
                            levelZeroHeight: canvas.imagePixelHeight
                        }

                        PrecisionCanvasPickerInput {
                            anchors.fill: parent
                            z: 100
                            editor: canvas.editor
                            previewImage: editedPreview
                            previewFrameReady: canvas.previewFrameReady
                            readyPreviewGeneration:
                                canvas.readyPreviewGeneration
                            displayScale: canvas.displayScale
                            interactionEnabled:
                                (canvas.editor.pointColorPickerActive
                                    || canvas.editor.whiteBalancePickerActive
                                    || canvas.editor.retouchPickerActive)
                                && !canvas.comparisonActive
                                && canvas.previewFrameReady
                                && canvas.readyPreviewGeneration.length > 0
                        }
                    }

                    ScrollBar.horizontal: ScrollBar {
                        policy: ScrollBar.AsNeeded
                        onPressedChanged: {
                            if (pressed) {
                                directViewportSettle.stop()
                            } else {
                                Qt.callLater(canvas.requestVisibleDetail)
                            }
                        }
                    }
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AsNeeded
                        onPressedChanged: {
                            if (pressed) {
                                directViewportSettle.stop()
                            } else {
                                Qt.callLater(canvas.requestVisibleDetail)
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
                    && !canvas.comparisonActive
                    && canvas.activeToolMode === canvas.toolNone
                toolActive: canvas.zoomToolActive
                fitView: canvas.fitView
                zoomFactor: canvas.zoomFactor
                fitZoomFactor: canvas.fitScale * canvas.deviceScale
                onZoomStepRequested: (viewportX, viewportY, direction) =>
                    canvas.zoomStepAtViewport(
                        viewportX, viewportY, direction)
                onContinuousZoomStarted: canvas.beginContinuousZoom()
                onContinuousZoomRequested:
                    (viewportX, viewportY, requestedZoom) =>
                        canvas.zoomAtViewport(
                            viewportX, viewportY, requestedZoom, false)
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
        }
