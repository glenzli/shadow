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
        && !showingFullDetail
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

    onZoomFactorChanged: viewStateChanged()
    onFitViewChanged: viewStateChanged()
    onComparisonActiveChanged: comparisonStateChanged()
    onComparisonModeChanged: comparisonStateChanged()
    onComparisonPositionChanged: comparisonStateChanged()
    onZebraEnabledChanged: analysisOverlayStateChanged()
    onActiveToolModeChanged: {
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

    function previewNormalizedPoint(sourceItem, sourceX, sourceY) {
        if (!previewFrameReadyState || readyPreviewGenerationState.length === 0
                || editedPreview.status !== Image.Ready)
            return null
        const mapped = editedPreview.mapFromItem(sourceItem, sourceX, sourceY)
        const paintedWidth = Math.max(1, editedPreview.paintedWidth)
        const paintedHeight = Math.max(1, editedPreview.paintedHeight)
        const paintedX = (editedPreview.width - paintedWidth) / 2
        const paintedY = (editedPreview.height - paintedHeight) / 2
        if (mapped.x < paintedX || mapped.y < paintedY
                || mapped.x > paintedX + paintedWidth
                || mapped.y > paintedY + paintedHeight)
            return null
        const normalizedX = Math.max(0, Math.min(
            1, (mapped.x - paintedX) / paintedWidth))
        const normalizedY = Math.max(0, Math.min(
            1, (mapped.y - paintedY) / paintedHeight))
        return Qt.point(normalizedX, normalizedY)
    }

    function pickPreviewColor(sourceItem, sourceX, sourceY) {
        const normalized = previewNormalizedPoint(sourceItem, sourceX, sourceY)
        if (normalized === null)
            return
        if (editor.retouchPickerActive)
            editor.addRetouchSpotFromPreview(normalized.x, normalized.y)
        else if (editor.whiteBalancePickerActive)
            editor.setWhiteBalanceFromPreview(
                normalized.x, normalized.y, readyPreviewGenerationState)
        else
            editor.addPointColorFromPreview(
                normalized.x, normalized.y, readyPreviewGenerationState)
    }

    function comparisonModeName(mode) {
        if (mode === comparisonWhole)
            return qsTr("Original only")
        if (mode === comparisonWipeVertical)
            return qsTr("Vertical wipe")
        if (mode === comparisonWipeHorizontal)
            return qsTr("Horizontal wipe")
        if (mode === comparisonSideBySide)
            return qsTr("Side by side")
        return qsTr("Top and bottom")
    }

    function comparisonModeIcon(mode) {
        if (mode === comparisonWipeVertical)
            return "qrc:/icons/compare-wipe-vertical.svg"
        if (mode === comparisonWipeHorizontal)
            return "qrc:/icons/compare-wipe-horizontal.svg"
        if (mode === comparisonSideBySide)
            return "qrc:/icons/compare-side-by-side.svg"
        if (mode === comparisonStacked)
            return "qrc:/icons/compare-stacked.svg"
        return "qrc:/icons/before-after.svg"
    }

    function activateComparison(mode) {
        resetView()
        comparisonMode = mode
        comparisonPosition = 0.5
        comparisonActive = true
        editor.requestBeforePreview()
    }

    function updateComparisonPosition(sourceItem, sourceX, sourceY) {
        const mapped = sourceItem.mapToItem(photoSurface, sourceX, sourceY)
        if (comparisonMode === comparisonWipeVertical) {
            comparisonPosition = Math.max(0.02, Math.min(
                0.98, mapped.x / Math.max(1, photoSurface.width)))
        } else if (comparisonMode === comparisonWipeHorizontal) {
            comparisonPosition = Math.max(0.02, Math.min(
                0.98, mapped.y / Math.max(1, photoSurface.height)))
        }
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
        const centerX = normalizedCenterX()
        const centerY = normalizedCenterY()
        fitView = false
        comparisonActive = false
        zoomFactor = value
        Qt.callLater(function() {
            canvas.centerOnNormalized(centerX, centerY)
            canvas.requestVisibleDetail()
        })
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

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 42
                    color: Theme.chrome
                    border.width: 0

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: canvas.frameBorderColor
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        spacing: 10

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                Layout.fillWidth: true
                                text: canvas.editor.active
                                    ? canvas.editor.title : qsTr("No photo open")
                                color: canvas.textPrimary
                                font.pixelSize: 12
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: canvas.editor.sourcePath
                                color: canvas.textMuted
                                font.pixelSize: 8
                                elide: Text.ElideMiddle
                            }
                        }

                        RowLayout {
                            spacing: 2

                            ShadowIconButton {
                                id: zebraButton
                                source: "qrc:/icons/zebra.svg"
                                variant: ShadowIconButton.Secondary
                                selected: canvas.zebraEnabled
                                toolTipText: qsTr("Toggle clipping warning · RAW uses sensor limits")
                                accessibleName: toolTipText
                                Accessible.checked: selected
                                enabled: canvas.editor.active
                                onClicked: canvas.zebraEnabled = !canvas.zebraEnabled
                            }

                            ShadowIconButton {
                                id: beforeAfterButton
                                source: canvas.comparisonModeIcon(
                                    canvas.comparisonMode)
                                variant: ShadowIconButton.Secondary
                                selected: canvas.comparisonActive
                                toolTipText: canvas.comparisonActive
                                    ? qsTr("Disable comparison")
                                    : qsTr("Compare with original · %1").arg(
                                        canvas.comparisonModeName(
                                            canvas.comparisonMode))
                                accessibleName: toolTipText
                                Accessible.checked: selected
                                enabled: canvas.editor.active
                                onClicked: {
                                    if (canvas.comparisonActive)
                                        canvas.comparisonActive = false
                                    else
                                        canvas.activateComparison(
                                            canvas.comparisonMode)
                                }
                            }

                            ShadowIconButton {
                                id: comparisonModeButton
                                source: "qrc:/icons/chevron-down.svg"
                                buttonSize: 24
                                iconSize: 12
                                toolTipText: qsTr("Choose comparison layout")
                                accessibleName: toolTipText
                                enabled: canvas.editor.active
                                onClicked: comparisonModePopup.open()

                                Popup {
                                    id: comparisonModePopup
                                    parent: comparisonModeButton
                                    x: Math.round((comparisonModeButton.width
                                        - width) / 2)
                                    y: comparisonModeButton.height + 6
                                    width: comparisonModeRow.implicitWidth + 16
                                    height: comparisonModeRow.implicitHeight + 16
                                    padding: 8
                                    modal: false
                                    closePolicy: Popup.CloseOnEscape
                                        | Popup.CloseOnPressOutside

                                    background: Rectangle {
                                        radius: Theme.controlRadius
                                        color: Theme.panelRaised
                                        border.width: 1
                                        border.color: Theme.borderStrong
                                    }

                                    contentItem: Row {
                                        id: comparisonModeRow
                                        spacing: 4

                                        Repeater {
                                            model: [
                                                { "mode": canvas.comparisonWhole,
                                                  "icon": "qrc:/icons/before-after.svg" },
                                                { "mode": canvas.comparisonWipeVertical,
                                                  "icon": "qrc:/icons/compare-wipe-vertical.svg" },
                                                { "mode": canvas.comparisonWipeHorizontal,
                                                  "icon": "qrc:/icons/compare-wipe-horizontal.svg" },
                                                { "mode": canvas.comparisonSideBySide,
                                                  "icon": "qrc:/icons/compare-side-by-side.svg" },
                                                { "mode": canvas.comparisonStacked,
                                                  "icon": "qrc:/icons/compare-stacked.svg" }
                                            ]

                                            delegate: ShadowIconButton {
                                                required property var modelData
                                                source: modelData.icon
                                                variant: ShadowIconButton.Secondary
                                                selected: canvas.comparisonMode
                                                    === modelData.mode
                                                toolTipText: canvas.comparisonModeName(
                                                    modelData.mode)
                                                accessibleName: toolTipText
                                                onClicked: {
                                                    comparisonModePopup.close()
                                                    canvas.activateComparison(
                                                        modelData.mode)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Label {
                            text: canvas.fitView
                                ? qsTr("FIT")
                                : qsTr("%L1%").arg(
                                    Math.round(canvas.zoomFactor * 100))
                            color: canvas.textMuted
                            font.family: "Menlo"
                            font.pixelSize: 9
                        }
                        ShadowInlineSlider {
                            id: zoomSlider
                            Layout.preferredWidth: 112
                            Layout.minimumWidth: 72
                            from: 0.25
                            to: 4.0
                            stepSize: 0.05
                            value: canvas.zoomFactor
                            enabled: canvas.editor.active
                                && !canvas.editor.stateBusy
                            onMoved: canvas.setPixelZoom(value)
                        }
                        ShadowButton {
                            id: actualPixelsButton
                            Layout.preferredWidth: 52
                            Layout.preferredHeight: 30
                            compact: true
                            variant: ShadowButton.Secondary
                            selected: !canvas.fitView
                                && Math.abs(canvas.zoomFactor - 1.0) < 0.001
                            text: qsTr("100%")
                            enabled: canvas.editor.active
                            onClicked: canvas.setPixelZoom(1.0)
                        }
                        ShadowIconButton {
                            id: fitButton
                            source: "qrc:/icons/fit-view.svg"
                            variant: ShadowIconButton.Secondary
                            selected: canvas.fitView
                            toolTipText: qsTr("Fit image to window")
                            accessibleName: toolTipText
                            Accessible.checked: selected
                            enabled: canvas.editor.active
                                && !canvas.editor.stateBusy
                            onClicked: canvas.resetView()
                        }
                    }
                }

                Flickable {
                    id: previewFlick
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    contentWidth: Math.max(width, photoSurface.width)
                    contentHeight: Math.max(height, photoSurface.height)
                    interactive: contentWidth > width || contentHeight > height
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

                        Item {
                            id: beforeClip
                            x: 0
                            y: 0
                            width: canvas.comparisonMode
                                === canvas.comparisonWipeVertical
                                ? photoSurface.width
                                    * canvas.comparisonPosition
                                : photoSurface.width
                            height: canvas.comparisonMode
                                === canvas.comparisonWipeHorizontal
                                ? photoSurface.height
                                    * canvas.comparisonPosition
                                : photoSurface.height
                            clip: true
                            visible: canvas.comparisonActive
                                && canvas.beforeReady
                            z: 20

                            Image {
                                id: beforePreviewImage
                                x: 0
                                y: 0
                                width: photoSurface.width
                                height: photoSurface.height
                                source: canvas.editor.beforePreviewSource
                                fillMode: Image.Stretch
                                asynchronous: true
                                cache: false
                                retainWhileLoading: true
                                smooth: true
                                onSourceChanged: canvas.beforeFrameReadyState = false
                                onStatusChanged: {
                                    if (status === Image.Ready)
                                        canvas.beforeFrameReadyState = true
                                    else if (status === Image.Null
                                            || status === Image.Error)
                                        canvas.beforeFrameReadyState = false
                                }
                            }
                        }

                        Rectangle {
                            id: dualCompareSurface
                            anchors.fill: parent
                            visible: canvas.dualComparison
                            color: Theme.photoCanvas
                            z: 30

                            Item {
                                id: dualBeforePane
                                x: 0
                                y: 0
                                width: canvas.comparisonMode
                                    === canvas.comparisonSideBySide
                                    ? parent.width / 2 : parent.width
                                height: canvas.comparisonMode
                                    === canvas.comparisonStacked
                                    ? parent.height / 2 : parent.height
                                clip: true

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    source: canvas.editor.beforePreviewSource
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                    cache: false
                                    retainWhileLoading: true
                                    smooth: true
                                }

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.margins: 14
                                    width: dualBeforeLabel.implicitWidth + 14
                                    height: 23
                                    radius: 4
                                    color: Theme.previewHudStrongOverlay
                                    border.color: Theme.previewHudBorder

                                    Label {
                                        id: dualBeforeLabel
                                        anchors.centerIn: parent
                                        text: qsTr("BEFORE")
                                        color: canvas.textSecondary
                                        font.pixelSize: 8
                                        font.weight: Font.Bold
                                        font.letterSpacing: 0.7
                                    }
                                }
                            }

                            Item {
                                id: dualAfterPane
                                x: canvas.comparisonMode
                                    === canvas.comparisonSideBySide
                                    ? parent.width / 2 : 0
                                y: canvas.comparisonMode
                                    === canvas.comparisonStacked
                                    ? parent.height / 2 : 0
                                width: canvas.comparisonMode
                                    === canvas.comparisonSideBySide
                                    ? parent.width / 2 : parent.width
                                height: canvas.comparisonMode
                                    === canvas.comparisonStacked
                                    ? parent.height / 2 : parent.height
                                clip: true

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    source: canvas.visiblePreviewSource
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                    cache: false
                                    retainWhileLoading: true
                                    smooth: true
                                }

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.margins: 14
                                    width: dualAfterLabel.implicitWidth + 14
                                    height: 23
                                    radius: 4
                                    color: Theme.previewHudStrongOverlay
                                    border.color: Theme.previewHudBorder

                                    Label {
                                        id: dualAfterLabel
                                        anchors.centerIn: parent
                                        text: qsTr("AFTER")
                                        color: canvas.textSecondary
                                        font.pixelSize: 8
                                        font.weight: Font.Bold
                                        font.letterSpacing: 0.7
                                    }
                                }
                            }

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 1
                                height: parent.height
                                visible: canvas.comparisonMode
                                    === canvas.comparisonSideBySide
                                color: Theme.previewHudBorder
                            }

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width
                                height: 1
                                visible: canvas.comparisonMode
                                    === canvas.comparisonStacked
                                color: Theme.previewHudBorder
                            }
                        }

                        Rectangle {
                            id: verticalComparisonDivider
                            x: Math.round(photoSurface.width
                                * canvas.comparisonPosition)
                            y: 0
                            width: 1
                            height: photoSurface.height
                            visible: canvas.comparisonActive
                                && canvas.beforeReady
                                && canvas.comparisonMode
                                    === canvas.comparisonWipeVertical
                            color: Theme.previewCompareDivider
                            z: 80

                            Rectangle {
                                anchors.centerIn: parent
                                width: 18
                                height: 32
                                radius: 9
                                color: Theme.previewHudStrongOverlay
                                border.width: 1
                                border.color: Theme.previewCompareDivider

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 2
                                    height: 14
                                    radius: 1
                                    color: Theme.previewCompareDivider
                                }
                            }

                            MouseArea {
                                id: verticalDividerDragArea
                                x: -12
                                y: 0
                                width: 25
                                height: parent.height
                                hoverEnabled: true
                                preventStealing: true
                                cursorShape: Qt.SizeHorCursor
                                onPressed: mouse => canvas.updateComparisonPosition(
                                    verticalDividerDragArea, mouse.x, mouse.y)
                                onPositionChanged: mouse => {
                                    if (pressed)
                                        canvas.updateComparisonPosition(
                                            verticalDividerDragArea,
                                            mouse.x, mouse.y)
                                }
                            }
                        }

                        Rectangle {
                            id: horizontalComparisonDivider
                            x: 0
                            y: Math.round(photoSurface.height
                                * canvas.comparisonPosition)
                            width: photoSurface.width
                            height: 1
                            visible: canvas.comparisonActive
                                && canvas.beforeReady
                                && canvas.comparisonMode
                                    === canvas.comparisonWipeHorizontal
                            color: Theme.previewCompareDivider
                            z: 80

                            Rectangle {
                                anchors.centerIn: parent
                                width: 32
                                height: 18
                                radius: 9
                                color: Theme.previewHudStrongOverlay
                                border.width: 1
                                border.color: Theme.previewCompareDivider

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 14
                                    height: 2
                                    radius: 1
                                    color: Theme.previewCompareDivider
                                }
                            }

                            MouseArea {
                                id: horizontalDividerDragArea
                                x: 0
                                y: -12
                                width: parent.width
                                height: 25
                                hoverEnabled: true
                                preventStealing: true
                                cursorShape: Qt.SizeVerCursor
                                onPressed: mouse => canvas.updateComparisonPosition(
                                    horizontalDividerDragArea, mouse.x, mouse.y)
                                onPositionChanged: mouse => {
                                    if (pressed)
                                        canvas.updateComparisonPosition(
                                            horizontalDividerDragArea,
                                            mouse.x, mouse.y)
                                }
                            }
                        }

                        Rectangle {
                            id: wipeBeforeBadge
                            x: 12
                            y: 12
                            width: wipeBeforeLabel.implicitWidth + 14
                            height: 23
                            radius: 4
                            visible: canvas.comparisonActive
                                && canvas.beforeReady
                                && (canvas.comparisonMode
                                    === canvas.comparisonWipeVertical
                                    || canvas.comparisonMode
                                        === canvas.comparisonWipeHorizontal)
                            color: Theme.previewHudStrongOverlay
                            border.color: Theme.previewHudBorder
                            z: 90

                            Label {
                                id: wipeBeforeLabel
                                anchors.centerIn: parent
                                text: qsTr("BEFORE")
                                color: canvas.textSecondary
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.7
                            }
                        }

                        Rectangle {
                            id: wipeAfterBadge
                            x: canvas.comparisonMode
                                === canvas.comparisonWipeVertical
                                ? photoSurface.width - width - 12 : 12
                            y: canvas.comparisonMode
                                === canvas.comparisonWipeHorizontal
                                ? photoSurface.height - height - 12 : 12
                            width: wipeAfterLabel.implicitWidth + 14
                            height: 23
                            radius: 4
                            visible: wipeBeforeBadge.visible
                            color: Theme.previewHudStrongOverlay
                            border.color: Theme.previewHudBorder
                            z: 90

                            Label {
                                id: wipeAfterLabel
                                anchors.centerIn: parent
                                text: qsTr("AFTER")
                                color: canvas.textSecondary
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.7
                            }
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

                        MouseArea {
                            id: pointColorPickArea
                            anchors.fill: parent
                            z: 100
                            enabled: (canvas.editor.pointColorPickerActive
                                    || canvas.editor.whiteBalancePickerActive
                                    || canvas.editor.retouchPickerActive)
                                && !canvas.comparisonActive
                                && canvas.previewFrameReady
                                && canvas.readyPreviewGeneration.length > 0
                            hoverEnabled: true
                            cursorShape: enabled ? Qt.BlankCursor : Qt.ArrowCursor
                            // Painting is a direct-manipulation gesture. Do
                            // not let the zoomable preview Flickable convert
                            // it into a pan after the first brush stamp.
                            preventStealing: true
                            property real pointerX: width / 2
                            property real pointerY: height / 2
                            property bool retouchBrushActive: false
                            property real lastRetouchStampX: -1
                            property real lastRetouchStampY: -1

                            function retouchBrushDiameter() {
                                // The persisted repair target has an 18px
                                // level-zero radius. Match its visible
                                // diameter rather than creating a second
                                // brush-size contract in the canvas.
                                return Math.max(18, 36 * canvas.displayScale)
                            }

                            function appendRetouchStamp(mouse, beginsStroke) {
                                const normalized = canvas.previewNormalizedPoint(
                                    pointColorPickArea, mouse.x, mouse.y)
                                if (normalized === null)
                                    return
                                const minimumSpacing = retouchBrushDiameter() * 0.45
                                if (!beginsStroke
                                        && Math.hypot(
                                            mouse.x - lastRetouchStampX,
                                            mouse.y - lastRetouchStampY
                                        ) < minimumSpacing) {
                                    return
                                }
                                canvas.editor.addRetouchSpotFromPreview(
                                    normalized.x, normalized.y)
                                lastRetouchStampX = mouse.x
                                lastRetouchStampY = mouse.y
                            }
                            onPositionChanged: mouse => {
                                pointerX = mouse.x
                                pointerY = mouse.y
                                if (pressed && retouchBrushActive)
                                    appendRetouchStamp(mouse, false)
                            }
                            onPressed: mouse => {
                                pointerX = mouse.x
                                pointerY = mouse.y
                                if (!canvas.editor.retouchPickerActive)
                                    return
                                retouchBrushActive = true
                                appendRetouchStamp(mouse, true)
                            }
                            onReleased: {
                                retouchBrushActive = false
                                lastRetouchStampX = -1
                                lastRetouchStampY = -1
                            }
                            onCanceled: {
                                retouchBrushActive = false
                                lastRetouchStampX = -1
                                lastRetouchStampY = -1
                            }
                            onClicked: mouse => {
                                if (!canvas.editor.retouchPickerActive) {
                                    canvas.pickPreviewColor(
                                        pointColorPickArea, mouse.x, mouse.y)
                                }
                            }
                        }

                        Item {
                            z: 101
                            visible: pointColorPickArea.enabled
                                && pointColorPickArea.containsMouse
                            x: pointColorPickArea.pointerX
                            y: pointColorPickArea.pointerY

                            Rectangle {
                                visible: canvas.editor.retouchPickerActive
                                anchors.centerIn: parent
                                width: Math.max(18, 36 * canvas.displayScale)
                                height: width
                                radius: width / 2
                                color: Theme.transparent
                                border.width: 1
                                border.color: Theme.previewCompareDivider

                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    radius: width / 2
                                    color: Theme.transparent
                                    border.width: 1
                                    border.color: Theme.accent
                                }
                            }

                            Item {
                                visible: !canvas.editor.retouchPickerActive
                                x: -6
                                y: -19
                                width: 24
                                height: 24

                                ShadowIcon {
                                    x: 1
                                    y: 1
                                    source: "qrc:/icons/eyedropper.svg"
                                    color: Theme.previewHudStrongOverlay
                                    size: 24
                                }
                                ShadowIcon {
                                    source: "qrc:/icons/eyedropper.svg"
                                    color: Theme.previewCompareDivider
                                    size: 24
                                }
                            }
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

            Rectangle {
                id: comparisonBadge
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: 58
                anchors.rightMargin: 14
                width: comparisonBadgeLabel.implicitWidth + 18
                height: 25
                radius: 4
                visible: canvas.editor.active
                    && (canvas.comparisonActive || canvas.showingFullDetail)
                color: Theme.previewHudOverlay
                border.color: canvas.comparisonActive
                    ? canvas.accent : Theme.previewHudBorder

                Label {
                    id: comparisonBadgeLabel
                    anchors.centerIn: parent
                    text: canvas.comparisonActive
                        ? canvas.comparisonMode === canvas.comparisonWhole
                            ? qsTr("BEFORE · NEUTRAL BASE")
                            : canvas.comparisonMode
                                === canvas.comparisonWipeVertical
                                ? qsTr("BEFORE / AFTER · VERTICAL WIPE")
                                : canvas.comparisonMode
                                    === canvas.comparisonWipeHorizontal
                                    ? qsTr("BEFORE / AFTER · HORIZONTAL WIPE")
                                    : canvas.comparisonMode
                                        === canvas.comparisonSideBySide
                                        ? qsTr("BEFORE / AFTER · SIDE BY SIDE")
                                        : qsTr("BEFORE / AFTER · TOP / BOTTOM")
                        : qsTr("AFTER · FULL-RES RGB DETAIL")
                    color: canvas.comparisonActive
                        ? canvas.accent : canvas.textSecondary
                    font.pixelSize: 8
                    font.weight: Font.Bold
                    font.letterSpacing: 0.7
                }
            }

            Rectangle {
                anchors.top: comparisonBadge.bottom
                anchors.right: comparisonBadge.right
                anchors.topMargin: 7
                width: Math.min(350, detailHintRow.implicitWidth + 20)
                height: 30
                radius: 4
                visible: !canvas.comparisonActive && !canvas.fitView
                    && canvas.zoomFactor >= 1.0
                    && ((canvas.editor.detailRendering && !canvas.detailImageReady)
                        || canvas.editor.detailErrorText.length > 0
                        || canvas.detailImageLoadFailed)
                color: Theme.previewHudStrongOverlay
                border.color: canvas.editor.detailErrorText.length > 0
                    || canvas.detailImageLoadFailed
                    ? Theme.errorBorder : canvas.frameBorderColor
                clip: true

                Row {
                    id: detailHintRow
                    anchors.centerIn: parent
                    spacing: 7
                    BusyIndicator {
                        width: 14
                        height: 14
                        visible: canvas.editor.detailRendering
                            && !canvas.detailImageReady
                        running: visible
                    }
                    Label {
                        width: Math.min(290, implicitWidth)
                        text: canvas.editor.detailErrorText.length > 0
                            ? canvas.editor.detailErrorText
                            : canvas.detailImageLoadFailed
                                ? qsTr("Full-detail viewport unavailable · showing proxy")
                                : qsTr("Preparing exact local full-resolution pixels…")
                        color: canvas.editor.detailErrorText.length > 0
                            || canvas.detailImageLoadFailed
                            ? Theme.errorText : canvas.textMuted
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }
                }
            }

            Rectangle {
                anchors.top: comparisonBadge.bottom
                anchors.right: comparisonBadge.right
                anchors.topMargin: 7
                width: Math.min(330, beforeHintRow.implicitWidth + 20)
                height: 30
                radius: 4
                visible: canvas.comparisonActive
                    && (!canvas.beforeReady || !canvas.beforeFrameReady)
                    && canvas.editor.active
                color: Theme.previewHudStrongOverlay
                border.color: canvas.frameBorderColor
                clip: true

                Row {
                    id: beforeHintRow
                    anchors.centerIn: parent
                    spacing: 7
                    BusyIndicator {
                        width: 14
                        height: 14
                        visible: canvas.editor.beforeRendering
                        running: visible
                    }
                    Label {
                        width: Math.min(270, implicitWidth)
                        text: canvas.editor.beforeErrorText.length > 0
                            ? canvas.editor.beforeErrorText
                            : canvas.editor.beforeRendering
                                ? qsTr("Preparing neutral import baseline…")
                                : qsTr("Waiting for the current preview…")
                        color: canvas.editor.beforeErrorText.length > 0
                            ? Theme.errorText : canvas.textMuted
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: 14
                visible: !canvas.previewFrameReady
                    && (canvas.editor.stateBusy
                        || canvas.editor.rendering
                        || (canvas.comparisonActive
                            && canvas.editor.beforeRendering))
                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: parent.visible
                }
                Label {
                    text: canvas.editor.active
                        ? qsTr("Rendering local edit") : qsTr("Opening photo")
                    color: canvas.textPrimary
                    font.pixelSize: 12
                }
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(390, parent.width - 60)
                spacing: 10
                visible: !canvas.editor.busy
                    && (!canvas.editor.active
                        || (!canvas.previewFrameReady
                            && editedPreview.status === Image.Error))
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: editedPreview.status === Image.Error
                        ? qsTr("PREVIEW ERROR") : qsTr("NO PHOTO OPEN")
                    color: editedPreview.status === Image.Error
                        ? Theme.errorText : canvas.textMuted
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.2
                }
                Label {
                    width: parent.width
                    text: canvas.editor.statusText
                    color: canvas.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    lineHeight: 1.35
                }
            }
        }
