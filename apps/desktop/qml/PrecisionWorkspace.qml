pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: precision

    required property var editor
    required property var lutLibrary
    required property var captureMetadata
    signal openLutLibraryRequested()
    signal openOpticsProfileLibraryRequested()
    signal returnToReviewRequested()
    property real zoomFactor: 1.0
    property bool fitView: true
    property bool comparisonActive: false
    property int comparisonMode: 1
    property real comparisonPosition: 0.5
    property real requestedDetailCenterX: 0.5
    property real requestedDetailCenterY: 0.5
    property bool detailImageReady: false
    property bool detailImageLoadFailed: false
    property bool componentReady: false
    property bool suppressViewportTracking: false
    property string readyPreviewGeneration: ""
    property bool previewFrameReady: false
    property bool beforeFrameReady: false
    property int mixerViewMode: 0
    property int selectedMixerBand: 0
    property int selectedSelectiveColorTarget: 0
    // These are display aids only. They do not mutate the edit stack, the
    // working recipe, or version history.
    property bool zebraEnabled: false
    property bool lumaWaveformEnabled: false

    readonly property int comparisonWhole: 0
    readonly property int comparisonWipeVertical: 1
    readonly property int comparisonWipeHorizontal: 2
    readonly property int comparisonSideBySide: 3
    readonly property int comparisonStacked: 4
    readonly property bool beforeReady: editor.beforePreviewSource.length > 0
    readonly property string visiblePreviewSource: editor.previewSource.length > 0
        ? editor.previewSource : editor.provisionalPreviewSource
    readonly property bool showingProvisionalPreview: editor.previewSource.length === 0
        && editor.provisionalPreviewSource.length > 0
    readonly property bool displayingBefore: comparisonActive && beforeReady
        && comparisonMode === comparisonWhole
    readonly property bool dualComparison: comparisonActive && beforeReady
        && (comparisonMode === comparisonSideBySide
            || comparisonMode === comparisonStacked)
    readonly property var displayedHistogram: displayingBefore
        ? editor.beforeHistogram : editor.histogram
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
    readonly property bool showingFullDetail: !fitView && zoomFactor >= 1.0
        && !comparisonActive && editor.detailMode && editor.detailTiles.length > 0
        && detailImageReady
    readonly property bool scopePreviewAvailable: editor.active
        && previewFrameReady
        && readyPreviewGeneration.length > 0
        && !showingProvisionalPreview
        && !comparisonActive
        && !showingFullDetail

    readonly property color panel: Theme.panel
    readonly property color panelRaised: Theme.panelRaised
    readonly property color border: Theme.border
    readonly property color textPrimary: Theme.textPrimary
    readonly property color textSecondary: Theme.textSecondary
    readonly property color textMuted: Theme.textMuted
    readonly property color accent: Theme.accent
    readonly property var colorMixerBands: [
        { "name": qsTr("Red"), "color": "#f04b4b", "hueLow": "#d94881", "hueHigh": "#f28a39", "oklchHue": 29.2339 },
        { "name": qsTr("Orange"), "color": "#f28a39", "hueLow": "#ef4c42", "hueHigh": "#e8c63c", "oklchHue": 52.9847 },
        { "name": qsTr("Yellow"), "color": "#e8c63c", "hueLow": "#f19a3d", "hueHigh": "#72bc4a", "oklchHue": 109.7692 },
        { "name": qsTr("Green"), "color": "#55b96c", "hueLow": "#b0c747", "hueHigh": "#35b6a4", "oklchHue": 142.4953 },
        { "name": qsTr("Aqua"), "color": "#32b8bd", "hueLow": "#43ae75", "hueHigh": "#3a8fdb", "oklchHue": 194.7689 },
        { "name": qsTr("Blue"), "color": "#477fdb", "hueLow": "#36a4d3", "hueHigh": "#755bd3", "oklchHue": 264.0520 },
        { "name": qsTr("Purple"), "color": "#8a5bcf", "hueLow": "#526fd9", "hueHigh": "#c34eb5", "oklchHue": 293.9376 },
        { "name": qsTr("Magenta"), "color": "#d04fa4", "hueLow": "#9856c9", "hueHigh": "#e34e73", "oklchHue": 328.3634 }
    ]
    readonly property var selectiveColorTargets: [
        { "name": qsTr("Reds"), "color": "#ef5b62" },
        { "name": qsTr("Yellows"), "color": "#e5bf45" },
        { "name": qsTr("Greens"), "color": "#4fb473" },
        { "name": qsTr("Cyans"), "color": "#37bdc7" },
        { "name": qsTr("Blues"), "color": "#5484d8" },
        { "name": qsTr("Magentas"), "color": "#cf5aa9" },
        { "name": qsTr("White"), "color": "#edf0f3" },
        { "name": qsTr("Neutral"), "color": "#8d98a5" },
        { "name": qsTr("Black"), "color": "#27313b" }
    ]

    function joinedIdentity(make, model) {
        const parts = []
        const cleanMake = String(make || "").trim()
        const cleanModel = String(model || "").trim()
        if (cleanMake.length > 0)
            parts.push(cleanMake)
        if (cleanModel.length > 0 && cleanModel !== cleanMake)
            parts.push(cleanModel)
        return parts.join(" ")
    }

    function formatShutter(seconds) {
        const value = Number(seconds)
        if (!(value > 0))
            return "—"
        if (value >= 1)
            return qsTr("%1 s").arg(
                value.toLocaleString(Qt.locale(), "f", value < 10 ? 1 : 0))
        const reciprocal = Math.round(1 / value)
        return reciprocal > 1 ? qsTr("1/%1 s").arg(reciprocal)
                              : qsTr("%1 s").arg(
                                  value.toLocaleString(Qt.locale(), "f", 2))
    }

    function captureSettingSummary() {
        if (!captureMetadata || !captureMetadata.available)
            return ""
        const values = []
        values.push(formatShutter(captureMetadata.exposureTimeSeconds))
        values.push(Number(captureMetadata.apertureFNumber) > 0
            ? qsTr("f/%1").arg(Number(captureMetadata.apertureFNumber)
                .toLocaleString(Qt.locale(), "f", 1)) : "—")
        values.push(Number(captureMetadata.isoSpeed) > 0
            ? qsTr("ISO %1").arg(Math.round(Number(captureMetadata.isoSpeed)))
            : "—")
        values.push(Number(captureMetadata.focalLengthMm) > 0
            ? qsTr("%1 mm").arg(Number(captureMetadata.focalLengthMm)
                .toLocaleString(Qt.locale(), "f", 1)) : "—")
        return values.join("   ·   ")
    }

    // A switch expresses the requested setting; this label expresses the result Lensfun actually
    // supplied for the currently rendered image. Keeping the two separate makes missing or
    // uncalibrated profile data immediately visible instead of making an enabled switch look
    // like proof that correction happened.
    function opticsEffectState(key) {
        const receipt = editor.opticsReceipt
        if (!receipt.valid || receipt.status !== "matched")
            return ""
        if (key === "master") {
            return receipt.appliedDistortion || receipt.appliedTca || receipt.appliedVignetting
                ? qsTr("Applied") : qsTr("No calibrated correction")
        }
        if (key === "distortion")
            return receipt.appliedDistortion ? qsTr("Applied") : qsTr("No data")
        if (key === "tca")
            return receipt.appliedTca ? qsTr("Applied") : qsTr("No data")
        if (key === "vignetting") {
            if (!receipt.appliedVignetting)
                return qsTr("No data")
            return receipt.vignettingUsedDistanceFallback
                ? qsTr("Applied · far focus") : qsTr("Applied")
        }
        return receipt.appliedScaling ? qsTr("Applied") : qsTr("Not needed")
    }

    function fineValue(key) {
        // Reading the revision makes generic key lookups reactive without
        // exposing dozens of one-off Q_PROPERTY accessors.
        const revision = editor.parameterRevision
        return revision >= 0 ? editor.parameterValue(key) : 0
    }

    function mixerComponent(tabIndex) {
        return tabIndex === 0 ? "hue"
            : tabIndex === 1 ? "saturation" : "lightness"
    }

    function mixerValue(index, component) {
        const revision = editor.parameterRevision
        return revision >= 0 ? editor.colorMixerValue(index, component) : 0
    }

    function selectiveColorValue(targetIndex, componentIndex) {
        const revision = editor.parameterRevision
        return revision >= 0 ? editor.selectiveColorValue(targetIndex, componentIndex) : 0
    }

    function selectiveColorRelative() {
        const revision = editor.parameterRevision
        return revision >= 0 ? editor.selectiveColorRelative() : true
    }

    function selectiveColorTrackStart(componentIndex) {
        if (componentIndex === 0)
            return "#e96870"
        if (componentIndex === 1)
            return "#58a976"
        if (componentIndex === 2)
            return "#5f88d4"
        return Theme.effectiveDark ? "#dce3ea" : "#ffffff"
    }

    function selectiveColorTrackEnd(componentIndex) {
        if (componentIndex === 0)
            return "#37bdc7"
        if (componentIndex === 1)
            return "#cf5aa9"
        if (componentIndex === 2)
            return "#e5bf45"
        return Theme.effectiveDark ? "#1c242c" : "#202830"
    }

    function mixerTrackStart(band, component) {
        if (component === "hue")
            return band.hueLow
        if (component === "saturation")
            return Theme.effectiveDark ? "#4b5055" : "#a9adb1"
        return Qt.darker(band.color, 3.2)
    }

    function mixerTrackMiddle(band, component) {
        if (component === "hue")
            return band.color
        if (component === "saturation")
            return Qt.darker(band.color, 1.35)
        return band.color
    }

    function mixerTrackEnd(band, component) {
        if (component === "hue")
            return band.hueHigh
        if (component === "saturation")
            return Qt.lighter(band.color, 1.18)
        return Qt.lighter(band.color, Theme.effectiveDark ? 1.9 : 1.55)
    }

    function pickPreviewColor(sourceItem, sourceX, sourceY) {
        if (!previewFrameReady || readyPreviewGeneration.length === 0
                || editedPreview.status !== Image.Ready)
            return
        const mapped = editedPreview.mapFromItem(sourceItem, sourceX, sourceY)
        const paintedWidth = Math.max(1, editedPreview.paintedWidth)
        const paintedHeight = Math.max(1, editedPreview.paintedHeight)
        const paintedX = (editedPreview.width - paintedWidth) / 2
        const paintedY = (editedPreview.height - paintedHeight) / 2
        if (mapped.x < paintedX || mapped.y < paintedY
                || mapped.x > paintedX + paintedWidth
                || mapped.y > paintedY + paintedHeight)
            return
        const normalizedX = Math.max(0, Math.min(
            1, (mapped.x - paintedX) / paintedWidth))
        const normalizedY = Math.max(0, Math.min(
            1, (mapped.y - paintedY) / paintedHeight))
        if (editor.whiteBalancePickerActive)
            editor.setWhiteBalanceFromPreview(
                normalizedX, normalizedY, readyPreviewGeneration)
        else
            editor.addPointColorFromPreview(
                normalizedX, normalizedY, readyPreviewGeneration)
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

    Connections {
        target: precision.editor
        function onSourcePathChanged() {
            precision.comparisonActive = false
            precision.previewFrameReady = false
            precision.beforeFrameReady = false
            precision.readyPreviewGeneration = ""
            precision.resetView()
        }
        function onDetailGeometryChanged() {
            if (!precision.fitView && precision.editor.detailMode) {
                Qt.callLater(function() {
                    precision.suppressViewportTracking = true
                    precision.centerOnNormalized(
                        precision.requestedDetailCenterX,
                        precision.requestedDetailCenterY
                    )
                    precision.suppressViewportTracking = false
                })
            }
        }
        function onDetailTilesChanged() {
            precision.detailImageReady = false
            precision.detailImageLoadFailed = false
        }
    }

    onDeviceScaleChanged: {
        if (!componentReady || fitView || zoomFactor < 1.0 || !editor.active)
            return
        editor.leaveDetailMode()
        Qt.callLater(function() {
            precision.centerOnNormalized(
                precision.requestedDetailCenterX,
                precision.requestedDetailCenterY
            )
            precision.requestVisibleDetail()
        })
    }

    Component.onCompleted: componentReady = true

    Timer {
        id: directViewportSettle
        interval: 70
        repeat: false
        onTriggered: precision.requestVisibleDetail()
    }

    function resetView() {
        fitView = true
        zoomFactor = 1.0
        detailImageReady = false
        detailImageLoadFailed = false
        previewFlick.contentX = 0
        previewFlick.contentY = 0
        editor.leaveDetailMode()
    }

    function previewGeneration(source) {
        const match = String(source).match(/[?&]generation=([^&#]+)/)
        return match && match.length > 1 ? decodeURIComponent(match[1]) : ""
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
            detailImageLoadFailed = true
            return
        }
        detailImageLoadFailed = false
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
                || (!editor.detailMode && !detailImageReady))
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
            precision.centerOnNormalized(centerX, centerY)
            precision.requestVisibleDetail()
        })
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
        sequence: "Y"
        enabled: precision.visible && precision.editor.active
        onActivated: {
            if (precision.comparisonActive)
                precision.comparisonActive = false
            else
                precision.activateComparison(precision.comparisonMode)
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: Math.max(220, Math.min(252, precision.width * 0.19))
            Layout.fillHeight: true
            color: precision.panel
            border.width: 0

            Rectangle {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 1
                color: precision.border
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.panelPadding
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("GRADE NODES")
                        color: precision.textSecondary
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.35
                    }
                    Label {
                        text: qsTr("%L1 / %L2")
                            .arg(precision.editor.gradeNodes.length).arg(16)
                        color: precision.textMuted
                        font.pixelSize: 10
                    }
                    ShadowIconButton {
                        id: addGradeNodeButton
                        source: "qrc:/icons/node-add.svg"
                        variant: ShadowIconButton.Secondary
                        foregroundColor: precision.accent
                        enabled: precision.editor.canAddGradeNode
                        toolTipText: qsTr("Add a neutral Grade Node after the selection")
                        accessibleName: qsTr("Add Grade Node")
                        onClicked: precision.editor.addGradeNode()
                    }
                }

                ListView {
                    id: gradeNodeList
                    objectName: "gradeNodeList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    model: precision.editor.gradeNodes
                    spacing: 6
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        id: gradeNodeRow
                        required property int index
                        required property var modelData

                        readonly property bool selected:
                            gradeNodeRow.index
                                === precision.editor.selectedGradeNodeIndex

                        width: gradeNodeList.width
                        height: 52
                        radius: 6
                        color: selected ? Theme.accentSurfaceQuiet : Theme.panelRaised
                        border.width: selected ? 1 : 0
                        border.color: Theme.accentBorder

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 7

                            Rectangle {
                                Layout.preferredWidth: 22
                                Layout.preferredHeight: 22
                                radius: 5
                                color: gradeNodeRow.selected
                                    ? Theme.accentSurface : Theme.surfaceSubtle
                                border.width: gradeNodeRow.selected ? 1 : 0
                                border.color: Theme.accentBorder
                                Label {
                                    anchors.centerIn: parent
                                    text: String(gradeNodeRow.index + 1).padStart(2, "0")
                                    color: gradeNodeRow.selected
                                        ? precision.accent : precision.textMuted
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    Layout.fillWidth: true
                                    text: gradeNodeRow.modelData.label
                                    color: gradeNodeRow.modelData.enabled
                                        ? precision.textPrimary : precision.textSecondary
                                    font.pixelSize: 11
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: gradeNodeRow.modelData.enabled
                                        ? qsTr("LOCAL GRADE · ENABLED")
                                        : qsTr("LOCAL GRADE · BYPASSED")
                                    color: gradeNodeRow.modelData.enabled
                                        ? (gradeNodeRow.selected
                                            ? Theme.accentTextMuted
                                            : precision.textMuted)
                                        : Theme.textMuted
                                    font.pixelSize: 9
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 0.2
                                    elide: Text.ElideRight
                                }
                            }

                            Switch {
                                id: rowEnabledSwitch
                                Layout.preferredWidth: 36
                                Layout.preferredHeight: 22
                                checked: gradeNodeRow.modelData.enabled
                                enabled: precision.editor.active && !precision.editor.stateBusy
                                Accessible.name: checked
                                    ? qsTr("Bypass %1").arg(gradeNodeRow.modelData.label)
                                    : qsTr("Enable %1").arg(gradeNodeRow.modelData.label)
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                ToolTip.text: checked
                                    ? qsTr("Bypass Grade Node; preserve all adjustments")
                                    : qsTr("Enable Grade Node")
                                onClicked: {
                                    precision.editor.selectGradeNode(gradeNodeRow.index)
                                    precision.editor.gradeNodeEnabled = checked
                                }
                                indicator: Rectangle {
                                    implicitWidth: 34
                                    implicitHeight: 18
                                    x: (rowEnabledSwitch.width - width) / 2
                                    y: (rowEnabledSwitch.height - height) / 2
                                    radius: height / 2
                                    color: rowEnabledSwitch.checked
                                        ? Theme.switchOnSurface : Theme.switchOffSurface
                                    border.color: rowEnabledSwitch.checked
                                        ? Theme.switchOnBorder : Theme.switchOffBorder
                                    Rectangle {
                                        width: 12
                                        height: 12
                                        y: 3
                                        x: rowEnabledSwitch.checked ? parent.width - width - 3 : 3
                                        radius: width / 2
                                        color: rowEnabledSwitch.checked
                                            ? precision.accent : precision.textMuted
                                    }
                                }
                                contentItem: Item {}
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            anchors.rightMargin: 44
                            cursorShape: Qt.PointingHandCursor
                            onClicked: precision.editor.selectGradeNode(
                                gradeNodeRow.index)
                        }
                    }

                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 5

                    Item { Layout.fillWidth: true }

                    ShadowIconButton {
                        id: copyGradeNodeButton
                        source: "qrc:/icons/duplicate.svg"
                        toolTipText: qsTr("Duplicate selected Grade Node")
                        accessibleName: toolTipText
                        enabled: precision.editor.hasSelectedGradeNode
                            && precision.editor.canAddGradeNode
                        onClicked: precision.editor.duplicateSelectedGradeNode()
                    }
                    ShadowIconButton {
                        id: deleteGradeNodeButton
                        source: "qrc:/icons/trash.svg"
                        toolTipText: qsTr("Delete selected Grade Node")
                        accessibleName: toolTipText
                        enabled: precision.editor.canDeleteGradeNode
                        onClicked: precision.editor.deleteSelectedGradeNode()
                    }
                    ShadowIconButton {
                        id: moveGradeNodeUpButton
                        source: "qrc:/icons/move-up.svg"
                        enabled: precision.editor.canMoveGradeNodeUp
                        toolTipText: qsTr("Move selected Grade Node up")
                        accessibleName: toolTipText
                        onClicked: precision.editor.moveSelectedGradeNode(
                            precision.editor.selectedGradeNodeIndex - 1
                        )
                    }
                    ShadowIconButton {
                        id: moveGradeNodeDownButton
                        source: "qrc:/icons/move-down.svg"
                        enabled: precision.editor.canMoveGradeNodeDown
                        toolTipText: qsTr("Move selected Grade Node down")
                        accessibleName: toolTipText
                        onClicked: precision.editor.moveSelectedGradeNode(
                            precision.editor.selectedGradeNodeIndex + 1
                        )
                    }

                    Item { Layout.fillWidth: true }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: precision.border
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Grade Nodes execute from top to bottom. Each node contains a complete, non-destructive grade.")
                    color: Theme.textSubtle
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    lineHeight: 1.35
                }
            }
        }

        Rectangle {
            id: precisionCanvas
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.photoCanvas

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
                        color: precision.border
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
                                text: precision.editor.active
                                    ? precision.editor.title : qsTr("No photo open")
                                color: precision.textPrimary
                                font.pixelSize: 12
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: precision.editor.sourcePath
                                color: precision.textMuted
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
                                selected: precision.zebraEnabled
                                toolTipText: qsTr("Toggle display zebra warning")
                                accessibleName: toolTipText
                                Accessible.checked: selected
                                enabled: precision.editor.active
                                onClicked: precision.zebraEnabled = !precision.zebraEnabled
                            }

                            ShadowIconButton {
                                id: lumaWaveformButton
                                source: "qrc:/icons/scopes.svg"
                                variant: ShadowIconButton.Secondary
                                selected: precision.lumaWaveformEnabled
                                toolTipText: qsTr("Toggle luma waveform")
                                accessibleName: toolTipText
                                Accessible.checked: selected
                                enabled: precision.editor.active
                                onClicked: precision.lumaWaveformEnabled
                                    = !precision.lumaWaveformEnabled
                            }

                            ShadowIconButton {
                                id: beforeAfterButton
                                source: precision.comparisonModeIcon(
                                    precision.comparisonMode)
                                variant: ShadowIconButton.Secondary
                                selected: precision.comparisonActive
                                toolTipText: precision.comparisonActive
                                    ? qsTr("Disable comparison")
                                    : qsTr("Compare with original · %1").arg(
                                        precision.comparisonModeName(
                                            precision.comparisonMode))
                                accessibleName: toolTipText
                                Accessible.checked: selected
                                enabled: precision.editor.active
                                onClicked: {
                                    if (precision.comparisonActive)
                                        precision.comparisonActive = false
                                    else
                                        precision.activateComparison(
                                            precision.comparisonMode)
                                }
                            }

                            ShadowIconButton {
                                id: comparisonModeButton
                                source: "qrc:/icons/chevron-down.svg"
                                buttonSize: 24
                                iconSize: 12
                                toolTipText: qsTr("Choose comparison layout")
                                accessibleName: toolTipText
                                enabled: precision.editor.active
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
                                                { "mode": precision.comparisonWhole,
                                                  "icon": "qrc:/icons/before-after.svg" },
                                                { "mode": precision.comparisonWipeVertical,
                                                  "icon": "qrc:/icons/compare-wipe-vertical.svg" },
                                                { "mode": precision.comparisonWipeHorizontal,
                                                  "icon": "qrc:/icons/compare-wipe-horizontal.svg" },
                                                { "mode": precision.comparisonSideBySide,
                                                  "icon": "qrc:/icons/compare-side-by-side.svg" },
                                                { "mode": precision.comparisonStacked,
                                                  "icon": "qrc:/icons/compare-stacked.svg" }
                                            ]

                                            delegate: ShadowIconButton {
                                                required property var modelData
                                                source: modelData.icon
                                                variant: ShadowIconButton.Secondary
                                                selected: precision.comparisonMode
                                                    === modelData.mode
                                                toolTipText: precision.comparisonModeName(
                                                    modelData.mode)
                                                accessibleName: toolTipText
                                                onClicked: {
                                                    comparisonModePopup.close()
                                                    precision.activateComparison(
                                                        modelData.mode)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Label {
                            text: precision.fitView
                                ? qsTr("FIT")
                                : qsTr("%L1%").arg(
                                    Math.round(precision.zoomFactor * 100))
                            color: precision.textMuted
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
                            value: precision.zoomFactor
                            enabled: precision.editor.active
                                && !precision.editor.stateBusy
                            onMoved: precision.setPixelZoom(value)
                        }
                        ShadowButton {
                            id: actualPixelsButton
                            Layout.preferredWidth: 52
                            Layout.preferredHeight: 30
                            compact: true
                            variant: ShadowButton.Secondary
                            selected: !precision.fitView
                                && Math.abs(precision.zoomFactor - 1.0) < 0.001
                            text: qsTr("100%")
                            enabled: precision.editor.active
                            onClicked: precision.setPixelZoom(1.0)
                        }
                        ShadowIconButton {
                            id: fitButton
                            source: "qrc:/icons/fit-view.svg"
                            variant: ShadowIconButton.Secondary
                            selected: precision.fitView
                            toolTipText: qsTr("Fit image to window")
                            accessibleName: toolTipText
                            Accessible.checked: selected
                            enabled: precision.editor.active
                                && !precision.editor.stateBusy
                            onClicked: precision.resetView()
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
                    onMovementEnded: precision.requestVisibleDetail()
                    onContentXChanged: precision.directViewportPositionChanged()
                    onContentYChanged: precision.directViewportPositionChanged()
                    onWidthChanged: {
                        if (precision.editor.detailMode)
                            precision.requestVisibleDetail()
                    }
                    onHeightChanged: {
                        if (precision.editor.detailMode)
                            precision.requestVisibleDetail()
                    }

                    Item {
                        id: photoSurface
                        x: (previewFlick.contentWidth - width) / 2
                        y: (previewFlick.contentHeight - height) / 2
                        width: precision.dualComparison
                            ? previewFlick.width
                            : precision.imagePixelWidth * precision.displayScale
                        height: precision.dualComparison
                            ? previewFlick.height
                            : precision.imagePixelHeight * precision.displayScale

                        Image {
                            id: editedPreview
                            anchors.fill: parent
                            source: precision.visiblePreviewSource
                            fillMode: Image.Stretch
                            asynchronous: true
                            cache: false
                            visible: !precision.dualComparison
                            // Keep the last decoded texture on screen until the
                            // replacement generation is actually ready. Without
                            // this, every slider update briefly exposes the
                            // canvas while Qt decodes the next JPEG.
                            retainWhileLoading: true
                            smooth: true
                            onSourceChanged: {
                                precision.previewFrameReady = false
                                precision.readyPreviewGeneration = ""
                            }
                            onStatusChanged: {
                                if (status === Image.Ready) {
                                    precision.previewFrameReady = true
                                    precision.readyPreviewGeneration
                                        = precision.previewGeneration(source)
                                } else if (status === Image.Null) {
                                    precision.previewFrameReady = false
                                    precision.readyPreviewGeneration = ""
                                } else if (status === Image.Error) {
                                    precision.readyPreviewGeneration = ""
                                }
                            }
                        }

                        Image {
                            id: displayZebraOverlay
                            anchors.fill: parent
                            source: precision.scopePreviewAvailable
                                ? "image://shadow-edit/scope/zebra/current?generation="
                                    + precision.readyPreviewGeneration : ""
                            fillMode: Image.Stretch
                            asynchronous: true
                            cache: false
                            retainWhileLoading: true
                            smooth: false
                            mipmap: false
                            visible: precision.zebraEnabled
                                && precision.scopePreviewAvailable
                                && status === Image.Ready
                            z: 10
                        }

                        Item {
                            id: beforeClip
                            x: 0
                            y: 0
                            width: precision.comparisonMode
                                === precision.comparisonWipeVertical
                                ? photoSurface.width
                                    * precision.comparisonPosition
                                : photoSurface.width
                            height: precision.comparisonMode
                                === precision.comparisonWipeHorizontal
                                ? photoSurface.height
                                    * precision.comparisonPosition
                                : photoSurface.height
                            clip: true
                            visible: precision.comparisonActive
                                && precision.beforeReady
                            z: 20

                            Image {
                                id: beforePreviewImage
                                x: 0
                                y: 0
                                width: photoSurface.width
                                height: photoSurface.height
                                source: precision.editor.beforePreviewSource
                                fillMode: Image.Stretch
                                asynchronous: true
                                cache: false
                                retainWhileLoading: true
                                smooth: true
                                onSourceChanged: precision.beforeFrameReady = false
                                onStatusChanged: {
                                    if (status === Image.Ready)
                                        precision.beforeFrameReady = true
                                    else if (status === Image.Null
                                            || status === Image.Error)
                                        precision.beforeFrameReady = false
                                }
                            }
                        }

                        Rectangle {
                            id: dualCompareSurface
                            anchors.fill: parent
                            visible: precision.dualComparison
                            color: Theme.photoCanvas
                            z: 30

                            Item {
                                id: dualBeforePane
                                x: 0
                                y: 0
                                width: precision.comparisonMode
                                    === precision.comparisonSideBySide
                                    ? parent.width / 2 : parent.width
                                height: precision.comparisonMode
                                    === precision.comparisonStacked
                                    ? parent.height / 2 : parent.height
                                clip: true

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    source: precision.editor.beforePreviewSource
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
                                        color: precision.textSecondary
                                        font.pixelSize: 8
                                        font.weight: Font.Bold
                                        font.letterSpacing: 0.7
                                    }
                                }
                            }

                            Item {
                                id: dualAfterPane
                                x: precision.comparisonMode
                                    === precision.comparisonSideBySide
                                    ? parent.width / 2 : 0
                                y: precision.comparisonMode
                                    === precision.comparisonStacked
                                    ? parent.height / 2 : 0
                                width: precision.comparisonMode
                                    === precision.comparisonSideBySide
                                    ? parent.width / 2 : parent.width
                                height: precision.comparisonMode
                                    === precision.comparisonStacked
                                    ? parent.height / 2 : parent.height
                                clip: true

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    source: precision.visiblePreviewSource
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
                                        color: precision.textSecondary
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
                                visible: precision.comparisonMode
                                    === precision.comparisonSideBySide
                                color: Theme.previewHudBorder
                            }

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width
                                height: 1
                                visible: precision.comparisonMode
                                    === precision.comparisonStacked
                                color: Theme.previewHudBorder
                            }
                        }

                        Rectangle {
                            id: verticalComparisonDivider
                            x: Math.round(photoSurface.width
                                * precision.comparisonPosition)
                            y: 0
                            width: 1
                            height: photoSurface.height
                            visible: precision.comparisonActive
                                && precision.beforeReady
                                && precision.comparisonMode
                                    === precision.comparisonWipeVertical
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
                                onPressed: mouse => precision.updateComparisonPosition(
                                    verticalDividerDragArea, mouse.x, mouse.y)
                                onPositionChanged: mouse => {
                                    if (pressed)
                                        precision.updateComparisonPosition(
                                            verticalDividerDragArea,
                                            mouse.x, mouse.y)
                                }
                            }
                        }

                        Rectangle {
                            id: horizontalComparisonDivider
                            x: 0
                            y: Math.round(photoSurface.height
                                * precision.comparisonPosition)
                            width: photoSurface.width
                            height: 1
                            visible: precision.comparisonActive
                                && precision.beforeReady
                                && precision.comparisonMode
                                    === precision.comparisonWipeHorizontal
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
                                onPressed: mouse => precision.updateComparisonPosition(
                                    horizontalDividerDragArea, mouse.x, mouse.y)
                                onPositionChanged: mouse => {
                                    if (pressed)
                                        precision.updateComparisonPosition(
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
                            visible: precision.comparisonActive
                                && precision.beforeReady
                                && (precision.comparisonMode
                                    === precision.comparisonWipeVertical
                                    || precision.comparisonMode
                                        === precision.comparisonWipeHorizontal)
                            color: Theme.previewHudStrongOverlay
                            border.color: Theme.previewHudBorder
                            z: 90

                            Label {
                                id: wipeBeforeLabel
                                anchors.centerIn: parent
                                text: qsTr("BEFORE")
                                color: precision.textSecondary
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.7
                            }
                        }

                        Rectangle {
                            id: wipeAfterBadge
                            x: precision.comparisonMode
                                === precision.comparisonWipeVertical
                                ? photoSurface.width - width - 12 : 12
                            y: precision.comparisonMode
                                === precision.comparisonWipeHorizontal
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
                                color: precision.textSecondary
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.7
                            }
                        }

                        Repeater {
                            model: precision.editor.detailTiles
                            delegate: Image {
                                required property var modelData
                                x: modelData.x * precision.displayScale
                                y: modelData.y * precision.displayScale
                                width: modelData.width * precision.displayScale
                                height: modelData.height * precision.displayScale
                                source: modelData.source
                                fillMode: Image.Stretch
                                asynchronous: true
                                cache: false
                                smooth: false
                                visible: precision.showingFullDetail
                                onStatusChanged: {
                                    if (status === Image.Ready)
                                        precision.detailImageReady = true
                                    else if (status === Image.Error) {
                                        precision.detailImageReady = false
                                        precision.detailImageLoadFailed = true
                                    }
                                }
                            }
                        }

                        MouseArea {
                            id: pointColorPickArea
                            anchors.fill: parent
                            z: 100
                            enabled: (precision.editor.pointColorPickerActive
                                    || precision.editor.whiteBalancePickerActive)
                                && !precision.comparisonActive
                                && precision.previewFrameReady
                                && precision.readyPreviewGeneration.length > 0
                            cursorShape: Qt.CrossCursor
                            onClicked: mouse => precision.pickPreviewColor(
                                pointColorPickArea, mouse.x, mouse.y)
                        }
                    }

                    ScrollBar.horizontal: ScrollBar {
                        policy: ScrollBar.AsNeeded
                        onPressedChanged: {
                            if (pressed) {
                                directViewportSettle.stop()
                            } else {
                                Qt.callLater(precision.requestVisibleDetail)
                            }
                        }
                    }
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AsNeeded
                        onPressedChanged: {
                            if (pressed) {
                                directViewportSettle.stop()
                            } else {
                                Qt.callLater(precision.requestVisibleDetail)
                            }
                        }
                    }
                }
            }

            Rectangle {
                id: lumaWaveformOverlay
                x: 18
                y: 58
                width: 356
                height: 207
                radius: Theme.controlRadius
                color: Theme.previewHudStrongOverlay
                border.width: 1
                border.color: Theme.previewHudBorder
                clip: true
                opacity: 0.82
                z: 180
                visible: precision.lumaWaveformEnabled
                    && precision.scopePreviewAvailable

                Rectangle {
                    id: waveformHeader
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 32
                    color: Theme.transparent

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 5
                        spacing: 7

                        ShadowIcon {
                            source: "qrc:/icons/scopes.svg"
                            color: precision.accent
                            size: 14
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("LUMA WAVEFORM")
                            color: precision.textPrimary
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.7
                        }
                        ShadowIconButton {
                            source: "qrc:/icons/clear.svg"
                            buttonSize: 22
                            iconSize: 12
                            toolTipText: qsTr("Hide luma waveform")
                            accessibleName: toolTipText
                            onClicked: precision.lumaWaveformEnabled = false
                        }
                    }

                    MouseArea {
                        id: waveformDragArea
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.rightMargin: 30
                        hoverEnabled: true
                        cursorShape: Qt.SizeAllCursor
                        drag.target: lumaWaveformOverlay
                        drag.axis: Drag.XAndYAxis
                        drag.minimumX: 12
                        drag.maximumX: Math.max(
                            12, precisionCanvas.width - lumaWaveformOverlay.width - 12)
                        drag.minimumY: 48
                        drag.maximumY: Math.max(
                            48, precisionCanvas.height - lumaWaveformOverlay.height - 12)
                    }
                }

                Item {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: waveformHeader.bottom
                    anchors.bottom: parent.bottom
                    anchors.margins: 10
                    clip: true

                    Repeater {
                        model: 5
                        delegate: Rectangle {
                            required property int index
                            x: Math.round(index * (parent.width - 1) / 4)
                            y: 0
                            width: 1
                            height: parent.height
                            color: Theme.previewHudBorder
                            opacity: 0.55
                        }
                    }
                    Repeater {
                        model: 4
                        delegate: Rectangle {
                            required property int index
                            x: 0
                            y: Math.round(index * (parent.height - 1) / 3)
                            width: parent.width
                            height: 1
                            color: Theme.previewHudBorder
                            opacity: 0.55
                        }
                    }
                    Image {
                        id: lumaWaveformImage
                        anchors.fill: parent
                        source: precision.scopePreviewAvailable
                            ? "image://shadow-edit/scope/waveform/current?generation="
                                + precision.readyPreviewGeneration : ""
                        fillMode: Image.Stretch
                        asynchronous: true
                        cache: false
                        retainWhileLoading: true
                        smooth: true
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
                visible: precision.editor.active
                    && (precision.comparisonActive
                        || precision.visiblePreviewSource.length > 0)
                color: Theme.previewHudOverlay
                border.color: precision.comparisonActive
                    ? precision.accent : Theme.previewHudBorder

                Label {
                    id: comparisonBadgeLabel
                    anchors.centerIn: parent
                    text: precision.comparisonActive
                        ? precision.comparisonMode === precision.comparisonWhole
                            ? qsTr("BEFORE · NEUTRAL BASE")
                            : precision.comparisonMode
                                === precision.comparisonWipeVertical
                                ? qsTr("BEFORE / AFTER · VERTICAL WIPE")
                                : precision.comparisonMode
                                    === precision.comparisonWipeHorizontal
                                    ? qsTr("BEFORE / AFTER · HORIZONTAL WIPE")
                                    : precision.comparisonMode
                                        === precision.comparisonSideBySide
                                        ? qsTr("BEFORE / AFTER · SIDE BY SIDE")
                                        : qsTr("BEFORE / AFTER · TOP / BOTTOM")
                        : precision.showingFullDetail
                            ? qsTr("AFTER · FULL-RES RGB DETAIL")
                            : precision.showingProvisionalPreview
                                ? qsTr("LIBRARY PREVIEW · DEVELOPING RAW")
                            : qsTr("AFTER · CURRENT EDIT PROXY")
                    color: precision.comparisonActive
                        ? precision.accent : precision.textSecondary
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
                visible: !precision.comparisonActive && !precision.fitView
                    && precision.zoomFactor >= 1.0
                    && ((precision.editor.detailRendering && !precision.detailImageReady)
                        || precision.editor.detailErrorText.length > 0
                        || precision.detailImageLoadFailed)
                color: Theme.previewHudStrongOverlay
                border.color: precision.editor.detailErrorText.length > 0
                    || precision.detailImageLoadFailed
                    ? Theme.errorBorder : precision.border
                clip: true

                Row {
                    id: detailHintRow
                    anchors.centerIn: parent
                    spacing: 7
                    BusyIndicator {
                        width: 14
                        height: 14
                        visible: precision.editor.detailRendering
                            && !precision.detailImageReady
                        running: visible
                    }
                    Label {
                        width: Math.min(290, implicitWidth)
                        text: precision.editor.detailErrorText.length > 0
                            ? precision.editor.detailErrorText
                            : precision.detailImageLoadFailed
                                ? qsTr("Full-detail viewport unavailable · showing proxy")
                                : qsTr("Preparing exact local full-resolution pixels…")
                        color: precision.editor.detailErrorText.length > 0
                            || precision.detailImageLoadFailed
                            ? Theme.errorText : precision.textMuted
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
                visible: precision.comparisonActive
                    && (!precision.beforeReady || !precision.beforeFrameReady)
                    && precision.editor.active
                color: Theme.previewHudStrongOverlay
                border.color: precision.border
                clip: true

                Row {
                    id: beforeHintRow
                    anchors.centerIn: parent
                    spacing: 7
                    BusyIndicator {
                        width: 14
                        height: 14
                        visible: precision.editor.beforeRendering
                        running: visible
                    }
                    Label {
                        width: Math.min(270, implicitWidth)
                        text: precision.editor.beforeErrorText.length > 0
                            ? precision.editor.beforeErrorText
                            : precision.editor.beforeRendering
                                ? qsTr("Preparing neutral import baseline…")
                                : qsTr("Waiting for the current preview…")
                        color: precision.editor.beforeErrorText.length > 0
                            ? Theme.errorText : precision.textMuted
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: 14
                visible: !precision.previewFrameReady
                    && (precision.editor.stateBusy
                        || precision.editor.rendering
                        || (precision.comparisonActive
                            && precision.editor.beforeRendering))
                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: parent.visible
                }
                Label {
                    text: precision.editor.active
                        ? qsTr("Rendering local edit") : qsTr("Opening photo")
                    color: precision.textPrimary
                    font.pixelSize: 12
                }
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(390, parent.width - 60)
                spacing: 10
                visible: !precision.editor.busy
                    && (!precision.editor.active
                        || (!precision.previewFrameReady
                            && editedPreview.status === Image.Error))
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: editedPreview.status === Image.Error
                        ? qsTr("PREVIEW ERROR") : qsTr("NO PHOTO OPEN")
                    color: editedPreview.status === Image.Error
                        ? Theme.errorText : precision.textMuted
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.2
                }
                Label {
                    width: parent.width
                    text: precision.editor.statusText
                    color: precision.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    lineHeight: 1.35
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: Math.max(304, Math.min(348, precision.width * 0.24))
            Layout.fillHeight: true
            color: precision.panel
            border.width: 0

            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: 1
                color: precision.border
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                EditHistogram {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 158
                    analysis: precision.displayedHistogram
                    beforeView: precision.displayingBefore
                    displayGeneration: precision.readyPreviewGeneration
                    panelColor: precision.panel
                    plotColor: Theme.plot
                    borderColor: Theme.transparent
                    textColor: precision.textPrimary
                    secondaryTextColor: precision.textSecondary
                    mutedTextColor: precision.textMuted
                    accentColor: precision.accent
                }

                Rectangle {
                    id: captureMetadataPanel
                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    color: precision.panel

                    readonly property bool metadataMatches:
                        precision.captureMetadata
                        && precision.captureMetadata.representationId
                            === precision.editor.representationId
                    readonly property bool metadataAvailable:
                        metadataMatches && precision.captureMetadata.available

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: precision.border
                    }

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 4

                        Label {
                            width: parent.width
                            text: captureMetadataPanel.metadataAvailable
                                ? precision.joinedIdentity(
                                    precision.captureMetadata.cameraMake,
                                    precision.captureMetadata.cameraModel)
                                : captureMetadataPanel.metadataMatches
                                    && precision.captureMetadata.pending
                                    ? qsTr("Preparing capture metadata…")
                                    : qsTr("Capture metadata unavailable")
                            color: captureMetadataPanel.metadataAvailable
                                ? precision.textPrimary : precision.textMuted
                            font.pixelSize: 10
                            font.weight: captureMetadataPanel.metadataAvailable
                                ? Font.Medium : Font.Normal
                            elide: Text.ElideRight
                        }

                        RowLayout {
                            width: parent.width
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                text: captureMetadataPanel.metadataAvailable
                                    ? precision.captureSettingSummary() : ""
                                color: precision.textSecondary
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.maximumWidth: parent.width * 0.42
                                visible: captureMetadataPanel.metadataAvailable
                                    && text.length > 0
                                text: precision.joinedIdentity(
                                    precision.captureMetadata.lensMake,
                                    precision.captureMetadata.lensModel)
                                color: precision.textMuted
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                TabBar {
                    id: rightTabs
                    Layout.fillWidth: true
                    Layout.topMargin: 2
                    Layout.preferredHeight: 36
                    spacing: 0
                    background: Rectangle {
                        color: Theme.transparent

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: precision.border
                        }
                    }

                    ShadowTabButton {
                        id: adjustTab
                        text: qsTr("ADJUST")
                        minimumTabWidth: 0
                        underlineInset: 32
                        underlineMaximumWidth: 52
                    }
                    ShadowTabButton {
                        id: versionsTab
                        text: qsTr("VERSIONS")
                        minimumTabWidth: 0
                        underlineInset: 32
                        underlineMaximumWidth: 52
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: rightTabs.currentIndex

                    Item {
                        ScrollView {
                            anchors.fill: parent
                            clip: true
                            contentWidth: availableWidth

                            ColumnLayout {
                                width: parent.width
                                spacing: 7
                                enabled: precision.editor.active && !precision.editor.stateBusy

                                Item { Layout.preferredHeight: 4 }

                                ColumnLayout {
                                    objectName: "gradeNodeInspector"
                                    Layout.fillWidth: true
                                    spacing: 8
                                    enabled: precision.editor.gradeNodeEnabled
                                    opacity: precision.editor.gradeNodeEnabled
                                        ? 1.0 : 0.42

                                    Behavior on opacity {
                                        NumberAnimation { duration: 100 }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 14
                                        Layout.rightMargin: 14
                                        text: precision.editor.hasSelectedGradeNode
                                            ? precision.editor.gradeNodes[
                                                precision.editor
                                                    .selectedGradeNodeIndex].label
                                            : qsTr("No Grade Node selected")
                                        color: precision.textPrimary
                                        font.pixelSize: 16
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 14
                                        Layout.rightMargin: 14
                                        text: qsTr("One complete, non-destructive adjustment. Light, tone, and color settings travel together when this node is copied, shared, or versioned.")
                                        color: precision.textMuted
                                        font.pixelSize: 10
                                        wrapMode: Text.WordWrap
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("WHITE BALANCE")

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            spacing: 6
                                            Item { Layout.fillWidth: true }
                                            ShadowIconButton {
                                                source: "qrc:/icons/eyedropper.svg"
                                                selected: precision.editor.whiteBalancePickerActive
                                                toolTipText: qsTr("Pick a neutral area for White Balance")
                                                accessibleName: toolTipText
                                                onClicked: precision.editor.setWhiteBalancePickerActive(
                                                    !precision.editor.whiteBalancePickerActive)
                                            }
                                        }

                                        ShadowSlider {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Temperature")
                                            from: -1.0
                                            to: 1.0
                                            neutralValue: 0.0
                                            stepSize: 0.005
                                            decimals: 0
                                            displayMultiplier: 100
                                            value: precision.editor.whiteBalanceTemperature
                                            semanticTrack: true
                                            trackStartColor: "#3979dc"
                                            trackMiddleColor: Theme.track
                                            trackEndColor: "#e49a3a"
                                            onGestureStarted: precision.editor.beginParameterEdit("white_balance_temperature")
                                            onEdited: value => precision.editor.whiteBalanceTemperature = value
                                            onGestureFinished: precision.editor.endParameterEdit("white_balance_temperature")
                                        }

                                        ShadowSlider {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Tint")
                                            from: -1.0
                                            to: 1.0
                                            neutralValue: 0.0
                                            stepSize: 0.005
                                            decimals: 0
                                            displayMultiplier: 100
                                            value: precision.editor.whiteBalanceTint
                                            semanticTrack: true
                                            trackStartColor: "#48a56a"
                                            trackMiddleColor: Theme.track
                                            trackEndColor: "#c65ab4"
                                            onGestureStarted: precision.editor.beginParameterEdit("white_balance_tint")
                                            onEdited: value => precision.editor.whiteBalanceTint = value
                                            onGestureFinished: precision.editor.endParameterEdit("white_balance_tint")
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("LIGHT")
                                        expanded: true

                                        ShadowSlider {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Exposure")
                                            from: -5.0
                                            to: 5.0
                                            neutralValue: 0.0
                                            stepSize: 0.05
                                            value: precision.editor.exposureStops
                                            suffix: " EV"
                                            onGestureStarted: precision.editor.beginParameterEdit("exposure")
                                            onEdited: value => precision.editor.exposureStops = value
                                            onGestureFinished: precision.editor.endParameterEdit("exposure")
                                        }

                                        ShadowSlider {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Contrast")
                                            from: 0.25
                                            to: 2.5
                                            neutralValue: 1.0
                                            stepSize: 0.01
                                            value: precision.editor.contrastFactor
                                            suffix: "×"
                                            onGestureStarted: precision.editor.beginParameterEdit("contrast")
                                            onEdited: value => precision.editor.contrastFactor = value
                                            onGestureFinished: precision.editor.endParameterEdit("contrast")
                                        }

                                        Repeater {
                                            model: [
                                                { "key": "highlights", "name": qsTr("Highlights") },
                                                { "key": "shadows", "name": qsTr("Shadows") },
                                                { "key": "whites", "name": qsTr("Whites") },
                                                { "key": "blacks", "name": qsTr("Blacks") }
                                            ]
                                            delegate: ShadowSlider {
                                                required property var modelData
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                label: modelData.name
                                                from: -1.0
                                                to: 1.0
                                                neutralValue: 0.0
                                                stepSize: 0.01
                                                decimals: 0
                                                displayMultiplier: 100
                                                suffix: "%"
                                                value: precision.fineValue(modelData.key)
                                                onGestureStarted: precision.editor.beginParameterEdit(modelData.key)
                                                onEdited: value => precision.editor.setParameterValue(modelData.key, value)
                                                onGestureFinished: precision.editor.endParameterEdit(modelData.key)
                                            }
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("COLOR")
                                        expanded: true

                                        ShadowSlider {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Chroma")
                                            from: 0.0
                                            to: 2.5
                                            neutralValue: 1.0
                                            stepSize: 0.01
                                            value: precision.editor.saturationFactor
                                            suffix: "×"
                                            onGestureStarted: precision.editor.beginParameterEdit("saturation")
                                            onEdited: value => precision.editor.saturationFactor = value
                                            onGestureFinished: precision.editor.endParameterEdit("saturation")
                                        }

                                        ShadowSlider {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Vibrance")
                                            from: -1.0
                                            to: 1.0
                                            neutralValue: 0.0
                                            stepSize: 0.01
                                            decimals: 0
                                            displayMultiplier: 100
                                            suffix: "%"
                                            value: precision.fineValue("vibrance")
                                            onGestureStarted: precision.editor.beginParameterEdit("vibrance")
                                            onEdited: value => precision.editor.setParameterValue("vibrance", value)
                                            onGestureFinished: precision.editor.endParameterEdit("vibrance")
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("TONE CURVE")

                                        ToneCurveEditor {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            Layout.preferredHeight: implicitHeight
                                            controller: precision.editor
                                            panelColor: precision.panelRaised
                                            plotColor: Theme.chrome
                                            borderColor: precision.border
                                            textColor: precision.textPrimary
                                            mutedTextColor: precision.textMuted
                                            accentColor: precision.accent
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("COLOR MIXER")

                                        TabBar {
                                            id: mixerViewTabs
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            Layout.preferredHeight: 28
                                            background: Rectangle {
                                                radius: Theme.controlRadius
                                                color: Theme.surfaceSubtle
                                                border.color: precision.border
                                            }
                                            onCurrentIndexChanged: precision.mixerViewMode = currentIndex
                                            ShadowTabButton { text: qsTr("OKLCH"); compact: true }
                                            ShadowTabButton { text: qsTr("COLOR"); compact: true }
                                        }

                                        TabBar {
                                            id: mixerTabs
                                            visible: precision.mixerViewMode === 0
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            Layout.preferredHeight: visible ? 28 : 0
                                            background: Item {}
                                            ShadowTabButton { text: qsTr("HUE"); compact: true }
                                            ShadowTabButton { text: qsTr("CHROMA"); compact: true }
                                            ShadowTabButton { text: qsTr("LIGHTNESS"); compact: true }
                                        }

                                        Repeater {
                                            model: precision.mixerViewMode === 0
                                                ? precision.colorMixerBands : []
                                            delegate: ShadowSlider {
                                                required property int index
                                                required property var modelData
                                                readonly property string component:
                                                    precision.mixerComponent(mixerTabs.currentIndex)
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                label: modelData.name
                                                accent: modelData.color
                                                semanticTrack: true
                                                trackStartColor: precision.mixerTrackStart(
                                                    modelData, component)
                                                trackMiddleColor: precision.mixerTrackMiddle(
                                                    modelData, component)
                                                trackEndColor: precision.mixerTrackEnd(
                                                    modelData, component)
                                                from: -1.0
                                                to: 1.0
                                                neutralValue: 0.0
                                                stepSize: 0.01
                                                decimals: 0
                                                displayMultiplier: 100
                                                suffix: "%"
                                                value: precision.mixerValue(index, component)
                                                onGestureStarted: precision.editor.beginParameterEdit(
                                                    "color_mixer/" + component + "/" + index)
                                                onEdited: value => precision.editor.setColorMixerValue(
                                                    index, component, value)
                                                onGestureFinished: precision.editor.endParameterEdit(
                                                    "color_mixer/" + component + "/" + index)
                                            }
                                        }

                                        RowLayout {
                                            visible: precision.mixerViewMode === 1
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 20
                                            Layout.rightMargin: 20
                                            Layout.topMargin: visible ? 7 : 0
                                            Layout.bottomMargin: visible ? 5 : 0
                                            Layout.preferredHeight: visible ? 30 : 0
                                            spacing: 8

                                            Item { Layout.fillWidth: true }

                                            Repeater {
                                                model: precision.colorMixerBands

                                                delegate: Rectangle {
                                                    required property int index
                                                    required property var modelData
                                                    Layout.preferredWidth: 18
                                                    Layout.preferredHeight: 18
                                                    Layout.alignment: Qt.AlignHCenter
                                                    radius: 9
                                                    color: modelData.color
                                                    border.width: precision.selectedMixerBand
                                                        === index ? 2 : 1
                                                    border.color: precision.selectedMixerBand === index
                                                        ? Theme.selectionForeground : Theme.borderStrong
                                                    opacity: precision.selectedMixerBand === index ? 1 : 0.72

                                                    TapHandler {
                                                        onTapped: precision.selectedMixerBand = parent.index
                                                    }

                                                    ToolTip.visible: swatchHover.hovered
                                                    ToolTip.delay: 450
                                                    ToolTip.text: modelData.name
                                                    HoverHandler { id: swatchHover }
                                                }
                                            }

                                            Item { Layout.fillWidth: true }
                                        }

                                        Repeater {
                                            model: precision.mixerViewMode === 1 ? [
                                                { "component": "hue", "name": qsTr("Hue") },
                                                { "component": "saturation", "name": qsTr("Chroma") },
                                                { "component": "lightness", "name": qsTr("Lightness") }
                                            ] : []

                                            delegate: ShadowSlider {
                                                required property int index
                                                required property var modelData
                                                readonly property int bandIndex:
                                                    precision.selectedMixerBand
                                                readonly property var band:
                                                    precision.colorMixerBands[bandIndex]
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                Layout.topMargin: index === 0 ? 4 : 0
                                                label: modelData.name
                                                accent: band.color
                                                semanticTrack: true
                                                trackStartColor: precision.mixerTrackStart(
                                                    band, modelData.component)
                                                trackMiddleColor: precision.mixerTrackMiddle(
                                                    band, modelData.component)
                                                trackEndColor: precision.mixerTrackEnd(
                                                    band, modelData.component)
                                                from: -1.0
                                                to: 1.0
                                                neutralValue: 0.0
                                                stepSize: 0.01
                                                decimals: 0
                                                displayMultiplier: 100
                                                suffix: "%"
                                                value: precision.mixerValue(
                                                    bandIndex, modelData.component)
                                                onGestureStarted: precision.editor.beginParameterEdit(
                                                    "color_mixer/" + modelData.component
                                                        + "/" + bandIndex)
                                                onEdited: value => precision.editor.setColorMixerValue(
                                                    bandIndex, modelData.component, value)
                                                onGestureFinished: precision.editor.endParameterEdit(
                                                    "color_mixer/" + modelData.component
                                                        + "/" + bandIndex)
                                            }
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("SELECTIVE COLOR")

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            Layout.topMargin: 4
                                            Layout.bottomMargin: 2
                                            spacing: 3

                                            Item { Layout.fillWidth: true }

                                            Repeater {
                                                model: precision.selectiveColorTargets

                                                delegate: ShadowColorLabelButton {
                                                    required property int index
                                                    required property var modelData
                                                    buttonSize: 25
                                                    labelColor: modelData.color
                                                    selected: precision.selectedSelectiveColorTarget === index
                                                    toolTipText: modelData.name
                                                    accessibleName: toolTipText
                                                    onClicked: precision.selectedSelectiveColorTarget = index
                                                }
                                            }

                                            Item { Layout.fillWidth: true }
                                        }

                                        TabBar {
                                            id: selectiveColorMethodTabs
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            Layout.topMargin: 3
                                            Layout.preferredHeight: 28
                                            currentIndex: precision.selectiveColorRelative() ? 0 : 1
                                            background: Rectangle {
                                                radius: Theme.controlRadius
                                                color: Theme.surfaceSubtle
                                                border.color: precision.border
                                            }
                                            onCurrentIndexChanged: {
                                                const relative = currentIndex === 0
                                                if (relative === precision.selectiveColorRelative())
                                                    return
                                                precision.editor.beginParameterEdit("selective_color/method")
                                                precision.editor.setSelectiveColorRelative(relative)
                                                precision.editor.endParameterEdit("selective_color/method")
                                            }
                                            ShadowTabButton {
                                                text: qsTr("RELATIVE")
                                                compact: true
                                                toolTipText: qsTr("Scale the existing CMYK component")
                                            }
                                            ShadowTabButton {
                                                text: qsTr("ABSOLUTE")
                                                compact: true
                                                toolTipText: qsTr("Add or remove a fixed CMYK amount")
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            Layout.topMargin: 3
                                            text: precision.selectiveColorTargets[
                                                precision.selectedSelectiveColorTarget].name
                                            color: precision.textSecondary
                                            font.pixelSize: 10
                                            font.weight: Font.DemiBold
                                            horizontalAlignment: Text.AlignRight
                                        }

                                        Repeater {
                                            model: [
                                                { "name": qsTr("Cyan"), "component": 0 },
                                                { "name": qsTr("Magenta"), "component": 1 },
                                                { "name": qsTr("Yellow"), "component": 2 },
                                                { "name": qsTr("Black"), "component": 3 }
                                            ]

                                            delegate: ShadowSlider {
                                                required property var modelData
                                                readonly property int targetIndex:
                                                    precision.selectedSelectiveColorTarget
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                label: modelData.name
                                                semanticTrack: true
                                                trackStartColor: precision.selectiveColorTrackStart(
                                                    modelData.component)
                                                trackMiddleColor: Theme.track
                                                trackEndColor: precision.selectiveColorTrackEnd(
                                                    modelData.component)
                                                from: -1.0
                                                to: 1.0
                                                neutralValue: 0.0
                                                stepSize: 0.01
                                                decimals: 0
                                                displayMultiplier: 100
                                                suffix: "%"
                                                value: precision.selectiveColorValue(
                                                    targetIndex, modelData.component)
                                                onGestureStarted: precision.editor.beginParameterEdit(
                                                    "selective_color/" + targetIndex
                                                        + "/" + modelData.component)
                                                onEdited: value => precision.editor.setSelectiveColorValue(
                                                    targetIndex, modelData.component, value)
                                                onGestureFinished: precision.editor.endParameterEdit(
                                                    "selective_color/" + targetIndex
                                                        + "/" + modelData.component)
                                            }
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("POINT COLOR")

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 20
                                            Layout.rightMargin: 20
                                            Layout.topMargin: 5
                                            Layout.bottomMargin: 6
                                            spacing: 8

                                            Label {
                                                text: qsTr("Samples")
                                                color: precision.textMuted
                                                font.pixelSize: 10
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 5

                                                Repeater {
                                                    model: precision.editor.pointColors

                                                    delegate: Rectangle {
                                                        required property var modelData
                                                        Layout.preferredWidth: 20
                                                        Layout.preferredHeight: 20
                                                        radius: 10
                                                        color: modelData.swatch
                                                        border.width: precision.editor.selectedPointColorIndex
                                                            === modelData.index ? 2 : 1
                                                        border.color: precision.editor.selectedPointColorIndex
                                                            === modelData.index
                                                            ? precision.accent : Theme.borderStrong

                                                        TapHandler {
                                                            onTapped: precision.editor.selectPointColor(
                                                                parent.modelData.index)
                                                        }
                                                    }
                                                }

                                                Label {
                                                    visible: precision.editor.pointColors.length === 0
                                                    text: qsTr("Pick one or more colors from the image")
                                                    color: Theme.textQuiet
                                                    font.pixelSize: 9
                                                    elide: Text.ElideRight
                                                    Layout.fillWidth: true
                                                }

                                                Item { Layout.fillWidth: true }
                                            }

                                            ShadowIconButton {
                                                source: "qrc:/icons/eyedropper.svg"
                                                selected: precision.editor.pointColorPickerActive
                                                enabled: precision.editor.active
                                                    && precision.editor.pointColors.length < 16
                                                    && precision.previewFrameReady
                                                    && precision.readyPreviewGeneration.length > 0
                                                    && !precision.comparisonActive
                                                toolTipText: qsTr("Add a Point Color sample from the image")
                                                accessibleName: toolTipText
                                                onClicked: precision.editor.setPointColorPickerActive(
                                                    !precision.editor.pointColorPickerActive)
                                            }

                                            ShadowIconButton {
                                                source: "qrc:/icons/trash.svg"
                                                variant: ShadowIconButton.Danger
                                                enabled: precision.editor.selectedPointColorIndex >= 0
                                                toolTipText: qsTr("Remove selected Point Color sample")
                                                accessibleName: toolTipText
                                                onClicked: precision.editor.removeSelectedPointColor()
                                            }
                                        }

                                        Rectangle {
                                            visible: precision.editor.selectedPointColorIndex >= 0
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 26
                                            Layout.rightMargin: 26
                                            Layout.topMargin: visible ? 5 : 0
                                            Layout.bottomMargin: visible ? 7 : 0
                                            Layout.preferredHeight: visible ? 8 : 0
                                            radius: 4
                                            gradient: Gradient {
                                                orientation: Gradient.Horizontal
                                                // Positions are Oklch hue angles / 360, not HSV
                                                // sextants. The colors are display-sRGB samples
                                                // whose linear values define the mixer anchors.
                                                GradientStop { position: 0.000000; color: "#eb6f9a" }
                                                GradientStop { position: 0.081205; color: "#ff0000" }
                                                GradientStop { position: 0.147180; color: "#ff8000" }
                                                GradientStop { position: 0.304914; color: "#ffff00" }
                                                GradientStop { position: 0.395820; color: "#00ff00" }
                                                GradientStop { position: 0.541025; color: "#00ffff" }
                                                GradientStop { position: 0.733478; color: "#0000ff" }
                                                GradientStop { position: 0.816493; color: "#8000ff" }
                                                GradientStop { position: 0.912121; color: "#ff00ff" }
                                                GradientStop { position: 1.000000; color: "#eb6f9a" }
                                            }
                                            Rectangle {
                                                x: Math.max(0, Math.min(parent.width - width,
                                                    ((precision.fineValue("color_range_center")
                                                        % 360) + 360) % 360
                                                        / 360 * parent.width - width / 2))
                                                y: -3
                                                width: 4
                                                height: parent.height + 6
                                                radius: 2
                                                color: Theme.selectionForeground
                                                border.color: Theme.accentHandleBorder
                                            }
                                        }

                                        Repeater {
                                            model: [
                                                { "key": "color_range_center", "name": qsTr("Target hue (OKLCh)"), "from": 0, "to": 360, "neutral": 0, "step": 1, "scale": 1, "suffix": "°" },
                                                { "key": "color_range_width", "name": qsTr("Range"), "from": 1, "to": 180, "neutral": 30, "step": 1, "scale": 1, "suffix": "°" },
                                                { "key": "color_range_softness", "name": qsTr("Softness"), "from": 0, "to": 1, "neutral": 0.5, "step": 0.01, "scale": 100, "suffix": "%" },
                                                { "key": "color_range_hue", "name": qsTr("Hue shift"), "from": -180, "to": 180, "neutral": 0, "step": 1, "scale": 1, "suffix": "°" },
                                                { "key": "color_range_saturation", "name": qsTr("Chroma"), "from": -1, "to": 1, "neutral": 0, "step": 0.01, "scale": 100, "suffix": "%" },
                                                { "key": "color_range_lightness", "name": qsTr("Lightness"), "from": -1, "to": 1, "neutral": 0, "step": 0.01, "scale": 100, "suffix": "%" }
                                            ]
                                            delegate: ShadowSlider {
                                                visible: precision.editor.selectedPointColorIndex >= 0
                                                required property var modelData
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                label: modelData.name
                                                from: modelData.from
                                                to: modelData.to
                                                neutralValue: modelData.neutral
                                                stepSize: modelData.step
                                                decimals: 0
                                                displayMultiplier: modelData.scale
                                                suffix: modelData.suffix
                                                value: precision.fineValue(modelData.key)
                                                enabled: precision.editor.selectedPointColorIndex >= 0
                                                onGestureStarted: precision.editor.beginParameterEdit(modelData.key)
                                                onEdited: value => precision.editor.setParameterValue(modelData.key, value)
                                                onGestureFinished: precision.editor.endParameterEdit(modelData.key)
                                            }
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("COLOR GRADING")

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 12
                                            Layout.rightMargin: 12
                                            spacing: 8

                                            Repeater {
                                                model: [
                                                    { "range": "shadows", "label": qsTr("Shadows"), "hue": "shadows_hue", "saturation": "shadows_saturation", "luminance": "shadows_luminance" },
                                                    { "range": "midtones", "label": qsTr("Midtones"), "hue": "midtones_hue", "saturation": "midtones_saturation", "luminance": "midtones_luminance" },
                                                    { "range": "highlights", "label": qsTr("Highlights"), "hue": "highlights_hue", "saturation": "highlights_saturation", "luminance": "highlights_luminance" }
                                                ]
                                                delegate: ShadowColorWheel {
                                                    required property var modelData
                                                    Layout.fillWidth: true
                                                    label: modelData.label
                                                    hue: precision.fineValue(modelData.hue)
                                                    saturation: precision.fineValue(modelData.saturation)
                                                    luminance: precision.fineValue(modelData.luminance)
                                                    onWheelGestureStarted: precision.editor.beginParameterEdit(
                                                        "color_grading/" + modelData.range + "/wheel")
                                                    onWheelEdited: (hue, saturation) =>
                                                        precision.editor.setColorGradingWheel(
                                                            modelData.range, hue, saturation)
                                                    onWheelGestureFinished: precision.editor.endParameterEdit(
                                                        "color_grading/" + modelData.range + "/wheel")
                                                    onLuminanceGestureStarted: precision.editor.beginParameterEdit(
                                                        modelData.luminance)
                                                    onLuminanceEdited: value =>
                                                        precision.editor.setParameterValue(
                                                            modelData.luminance, value)
                                                    onLuminanceGestureFinished: precision.editor.endParameterEdit(
                                                        modelData.luminance)
                                                }
                                            }
                                        }

                                        Repeater {
                                            model: [
                                                { "key": "grading_blending", "name": qsTr("Blending"), "from": 0, "neutral": 0.5 },
                                                { "key": "grading_balance", "name": qsTr("Balance"), "from": -1, "neutral": 0 }
                                            ]
                                            delegate: ShadowSlider {
                                                required property var modelData
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                label: modelData.name
                                                from: modelData.from; to: 1
                                                neutralValue: modelData.neutral
                                                stepSize: 0.01; decimals: 0
                                                displayMultiplier: 100; suffix: "%"
                                                value: precision.fineValue(modelData.key)
                                                onGestureStarted: precision.editor.beginParameterEdit(modelData.key)
                                                onEdited: value => precision.editor.setParameterValue(modelData.key, value)
                                                onGestureFinished: precision.editor.endParameterEdit(modelData.key)
                                            }
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        id: lutSection
                                        Layout.fillWidth: true
                                        title: qsTr("LUT")
                                        summary: precision.editor.hasLut
                                            ? precision.editor.lutTitle : qsTr("None")

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            spacing: 6

                                            Rectangle {
                                                id: lutSelector
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 52
                                                radius: Theme.controlRadius
                                                color: lutSelectorMouse.pressed
                                                    ? Theme.buttonPressedSurface
                                                    : lutSelectorMouse.containsMouse
                                                        ? Theme.buttonHoverSurface
                                                        : Theme.buttonSurface
                                                border.width: 1
                                                border.color: lutSelectorMouse.containsMouse
                                                    ? Theme.borderStrong : Theme.buttonBorder

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 10
                                                    anchors.rightMargin: 9
                                                    spacing: 8

                                                    Rectangle {
                                                        Layout.preferredWidth: 62
                                                        Layout.preferredHeight: 38
                                                        radius: Theme.compactControlRadius
                                                        clip: true
                                                        color: Theme.photoCanvas
                                                        border.color: Theme.border

                                                        Image {
                                                            anchors.fill: parent
                                                            source: "image://shadow-lut/"
                                                                + (precision.editor.hasLut
                                                                    ? precision.editor.lutResourceId
                                                                    : "original")
                                                            sourceSize.width: 124
                                                            sourceSize.height: 76
                                                            asynchronous: true
                                                            cache: true
                                                            fillMode: Image.PreserveAspectCrop
                                                        }
                                                    }

                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: precision.editor.hasLut
                                                            ? precision.editor.lutTitle
                                                            : qsTr("Choose a LUT")
                                                        color: precision.editor.hasLut
                                                            ? precision.textPrimary
                                                            : precision.textMuted
                                                        font.pixelSize: 10
                                                        elide: Text.ElideRight
                                                    }

                                                    ShadowIcon {
                                                        Layout.preferredWidth: 14
                                                        Layout.preferredHeight: 14
                                                        size: 14
                                                        source: "qrc:/icons/chevron-down.svg"
                                                        color: precision.textSecondary
                                                        rotation: lutPicker.opened ? 180 : 0
                                                    }
                                                }

                                                MouseArea {
                                                    id: lutSelectorMouse
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    enabled: precision.editor.active
                                                        && !precision.editor.stateBusy
                                                    onClicked: lutPicker.open()
                                                }

                                                Popup {
                                                    id: lutPicker
                                                    parent: lutSelector
                                                    x: 0
                                                    y: lutSelector.height + 5
                                                    width: Math.max(lutSelector.width, 270)
                                                    height: Math.min(360,
                                                        68 + Math.max(1,
                                                            precision.lutLibrary.availableEntries.length) * 62)
                                                    padding: 5
                                                    modal: false
                                                    closePolicy: Popup.CloseOnEscape
                                                        | Popup.CloseOnPressOutside

                                                    background: Rectangle {
                                                        radius: Theme.controlRadius
                                                        color: Theme.panelRaised
                                                        border.width: 1
                                                        border.color: Theme.borderStrong
                                                    }

                                                    contentItem: ListView {
                                                        id: lutPickerList
                                                        clip: true
                                                        spacing: 2
                                                        model: precision.lutLibrary.availableEntries

                                                        header: Rectangle {
                                                            width: lutPickerList.width
                                                            height: 60
                                                            radius: Theme.compactControlRadius
                                                            color: noneLutMouse.containsMouse
                                                                ? Theme.buttonGhostHover
                                                                : Theme.transparent

                                                            RowLayout {
                                                                anchors.fill: parent
                                                                anchors.leftMargin: 7
                                                                anchors.rightMargin: 9
                                                                spacing: 9

                                                                Rectangle {
                                                                    Layout.preferredWidth: 68
                                                                    Layout.preferredHeight: 44
                                                                    radius: Theme.compactControlRadius
                                                                    clip: true
                                                                    color: Theme.photoCanvas
                                                                    border.color: Theme.border

                                                                    Image {
                                                                        anchors.fill: parent
                                                                        source: "image://shadow-lut/original"
                                                                        sourceSize.width: 136
                                                                        sourceSize.height: 88
                                                                        asynchronous: true
                                                                        cache: true
                                                                        fillMode: Image.PreserveAspectCrop
                                                                    }
                                                                }

                                                                Label {
                                                                    Layout.fillWidth: true
                                                                    text: qsTr("No LUT")
                                                                    color: precision.editor.hasLut
                                                                        ? precision.textSecondary
                                                                        : precision.accent
                                                                    font.pixelSize: 10
                                                                    font.weight: precision.editor.hasLut
                                                                        ? Font.Normal : Font.DemiBold
                                                                }
                                                            }

                                                            MouseArea {
                                                                id: noneLutMouse
                                                                anchors.fill: parent
                                                                hoverEnabled: true
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: {
                                                                    precision.editor.clearLut()
                                                                    lutPicker.close()
                                                                }
                                                            }
                                                        }

                                                        delegate: Rectangle {
                                                            id: lutOptionRow
                                                            required property var modelData
                                                            width: lutPickerList.width
                                                            height: 60
                                                            radius: Theme.compactControlRadius
                                                            readonly property bool current:
                                                                precision.editor.lutResourceId
                                                                    === modelData.id
                                                            color: current
                                                                ? Theme.accentSurfaceQuiet
                                                                : lutEntryMouse.containsMouse
                                                                    ? Theme.buttonGhostHover
                                                                    : Theme.transparent

                                                            RowLayout {
                                                                anchors.fill: parent
                                                                anchors.leftMargin: 10
                                                                anchors.rightMargin: 8
                                                                spacing: 8

                                                                Rectangle {
                                                                    Layout.preferredWidth: 68
                                                                    Layout.preferredHeight: 44
                                                                    radius: Theme.compactControlRadius
                                                                    clip: true
                                                                    color: Theme.photoCanvas
                                                                    border.color: lutOptionRow.current
                                                                        ? Theme.accentBorder : Theme.border

                                                                    Image {
                                                                        anchors.fill: parent
                                                                        source: "image://shadow-lut/"
                                                                            + lutOptionRow.modelData.id
                                                                        sourceSize.width: 136
                                                                        sourceSize.height: 88
                                                                        asynchronous: true
                                                                        cache: true
                                                                        fillMode: Image.PreserveAspectCrop
                                                                    }
                                                                }

                                                                ColumnLayout {
                                                                    Layout.fillWidth: true
                                                                    spacing: 1

                                                                    Label {
                                                                        Layout.fillWidth: true
                                                                        text: lutOptionRow.modelData.title
                                                                        color: lutOptionRow.current
                                                                            ? precision.accent
                                                                            : precision.textPrimary
                                                                        font.pixelSize: 10
                                                                        font.weight: lutOptionRow.current
                                                                            ? Font.DemiBold : Font.Normal
                                                                        elide: Text.ElideRight
                                                                    }
                                                                    Label {
                                                                        text: qsTr("%1³").arg(
                                                                            lutOptionRow.modelData.size)
                                                                        color: precision.textMuted
                                                                        font.pixelSize: 9
                                                                    }
                                                                }
                                                            }

                                                            MouseArea {
                                                                id: lutEntryMouse
                                                                anchors.fill: parent
                                                                hoverEnabled: true
                                                                cursorShape: Qt.PointingHandCursor
                                                                onClicked: {
                                                                    precision.editor.setLutResource(
                                                                        lutOptionRow.modelData.id,
                                                                        lutOptionRow.modelData.title,
                                                                        lutOptionRow.modelData.managedPath)
                                                                    lutPicker.close()
                                                                }
                                                            }
                                                        }

                                                        Label {
                                                            anchors.centerIn: parent
                                                            visible: precision.lutLibrary.availableEntries.length === 0
                                                            text: qsTr("No LUTs in the Library")
                                                            color: precision.textMuted
                                                            font.pixelSize: 10
                                                        }
                                                    }
                                                }
                                            }

                                            ShadowIconButton {
                                                Layout.preferredWidth: Theme.controlHeight
                                                Layout.preferredHeight: Theme.controlHeight
                                                Layout.alignment: Qt.AlignVCenter
                                                buttonSize: Theme.controlHeight
                                                variant: ShadowIconButton.Secondary
                                                source: "qrc:/icons/library-manage.svg"
                                                toolTipText: qsTr("Manage LUT Library")
                                                onClicked: precision.openLutLibraryRequested()
                                            }

                                            ShadowIconButton {
                                                visible: precision.editor.hasLut
                                                Layout.preferredWidth: visible
                                                    ? Theme.controlHeight : 0
                                                Layout.preferredHeight: Theme.controlHeight
                                                Layout.alignment: Qt.AlignVCenter
                                                buttonSize: Theme.controlHeight
                                                variant: ShadowIconButton.Ghost
                                                source: "qrc:/icons/clear.svg"
                                                toolTipText: qsTr("Remove LUT from this Grade Node")
                                                onClicked: precision.editor.clearLut()
                                            }
                                        }

                                        ShadowSlider {
                                            visible: precision.editor.hasLut
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Intensity")
                                            from: 0
                                            to: 1
                                            neutralValue: 1
                                            stepSize: 0.01
                                            decimals: 0
                                            displayMultiplier: 100
                                            suffix: "%"
                                            value: precision.editor.lutIntensity
                                            onGestureStarted: precision.editor.beginParameterEdit(
                                                "lut_intensity")
                                            onEdited: value => precision.editor.lutIntensity = value
                                            onGestureFinished: precision.editor.endParameterEdit(
                                                "lut_intensity")
                                        }

                                        RowLayout {
                                            visible: precision.lutLibrary.availableEntries.length === 0
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14

                                            Label {
                                                Layout.fillWidth: true
                                                text: qsTr("Add .cube folders in the LUT Library first")
                                                color: precision.textMuted
                                                font.pixelSize: 9
                                            }
                                            Label {
                                                text: qsTr("MANAGE")
                                                color: precision.accent
                                                font.pixelSize: 9
                                                font.weight: Font.DemiBold

                                                TapHandler {
                                                    onTapped: precision.openLutLibraryRequested()
                                                }
                                            }
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("DETAIL")

                                        Label {
                                            Layout.leftMargin: 14
                                            text: qsTr("SHARPENING")
                                            color: Theme.textMuted
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                            font.letterSpacing: 0.7
                                        }

                                        Repeater {
                                            model: [
                                                { "key": "sharpen_amount", "name": qsTr("Amount"), "from": 0, "to": 2, "neutral": 0, "step": 0.01, "decimals": 0, "scale": 100, "suffix": "%" },
                                                { "key": "sharpen_radius", "name": qsTr("Radius"), "from": 0.1, "to": 5, "neutral": 1, "step": 0.1, "decimals": 1, "scale": 1, "suffix": " px" },
                                                { "key": "sharpen_threshold", "name": qsTr("Threshold"), "from": 0, "to": 1, "neutral": 0, "step": 0.01, "decimals": 0, "scale": 100, "suffix": "%" },
                                                { "key": "sharpen_masking", "name": qsTr("Masking"), "from": 0, "to": 1, "neutral": 0, "step": 0.01, "decimals": 0, "scale": 100, "suffix": "%" }
                                            ]
                                            delegate: ShadowSlider {
                                                required property var modelData
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                label: modelData.name
                                                from: modelData.from
                                                to: modelData.to
                                                neutralValue: modelData.neutral
                                                stepSize: modelData.step
                                                decimals: modelData.decimals
                                                displayMultiplier: modelData.scale
                                                suffix: modelData.suffix
                                                value: precision.fineValue(modelData.key)
                                                onGestureStarted: precision.editor.beginParameterEdit(modelData.key)
                                                onEdited: value => precision.editor.setParameterValue(modelData.key, value)
                                                onGestureFinished: precision.editor.endParameterEdit(modelData.key)
                                            }
                                        }

                                        Label {
                                            Layout.leftMargin: 14
                                            Layout.topMargin: 4
                                            text: qsTr("NOISE REDUCTION")
                                            color: Theme.textMuted
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                            font.letterSpacing: 0.7
                                        }

                                        Repeater {
                                            model: [
                                                { "key": "denoise_luminance", "name": qsTr("Luminance"), "neutral": 0 },
                                                { "key": "denoise_detail", "name": qsTr("Detail"), "neutral": 0.5 },
                                                { "key": "denoise_color", "name": qsTr("Color"), "neutral": 0 }
                                            ]
                                            delegate: ShadowSlider {
                                                required property var modelData
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 14
                                                Layout.rightMargin: 14
                                                label: modelData.name
                                                from: 0; to: 1; neutralValue: modelData.neutral
                                                stepSize: 0.01; decimals: 0
                                                displayMultiplier: 100; suffix: "%"
                                                value: precision.fineValue(modelData.key)
                                                onGestureStarted: precision.editor.beginParameterEdit(modelData.key)
                                                onEdited: value => precision.editor.setParameterValue(modelData.key, value)
                                                onGestureFinished: precision.editor.endParameterEdit(modelData.key)
                                            }
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("OPTICS")

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            spacing: 5

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 6
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: {
                                                    const receipt = precision.editor.opticsReceipt
                                                    if (!receipt.valid)
                                                        return qsTr("Preparing lens profile…")
                                                    if (receipt.status === "matched")
                                                        return receipt.lensProfile.length > 0
                                                            ? receipt.lensProfile
                                                            : qsTr("Lens profile matched")
                                                    if (receipt.status === "disabled")
                                                        return qsTr("Automatic correction is bypassed")
                                                    if (receipt.status === "provider_unavailable")
                                                        return qsTr("Lensfun provider unavailable")
                                                    if (receipt.status === "camera_not_found")
                                                        return qsTr("Camera profile not found")
                                                    if (receipt.status === "lens_not_found")
                                                        return qsTr("Lens profile not found")
                                                    return qsTr("Insufficient lens metadata")
                                                    }
                                                    color: precision.editor.opticsReceipt.status === "matched"
                                                        ? Theme.successText : Theme.textMuted
                                                    font.pixelSize: 10
                                                    elide: Text.ElideRight
                                                }
                                                ShadowIconButton {
                                                    source: "qrc:/icons/library-manage.svg"
                                                    buttonSize: 26
                                                    toolTipText: qsTr("Choose optical profile")
                                                    accessibleName: toolTipText
                                                    onClicked: precision.openOpticsProfileLibraryRequested()
                                                }
                                            }

                                            Repeater {
                                                model: [
                                                    { "key": "master", "name": qsTr("Automatic lens correction") },
                                                    { "key": "distortion", "name": qsTr("Distortion") },
                                                    { "key": "tca", "name": qsTr("Chromatic aberration") },
                                                    { "key": "vignetting", "name": qsTr("Lens vignetting") },
                                                    { "key": "scale", "name": qsTr("Automatic crop") }
                                                ]
                                                delegate: RowLayout {
                                                    required property var modelData
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 24
                                                    spacing: 8

                                                    readonly property bool optionChecked:
                                                        modelData.key === "master" ? precision.editor.opticsEnabled
                                                        : modelData.key === "distortion" ? precision.editor.opticsDistortionEnabled
                                                        : modelData.key === "tca" ? precision.editor.opticsTcaEnabled
                                                        : modelData.key === "vignetting" ? precision.editor.opticsVignettingEnabled
                                                        : precision.editor.opticsAutomaticScale

                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: parent.modelData.name
                                                        color: parent.enabled ? Theme.textSecondary : Theme.textDisabled
                                                        font.pixelSize: 10
                                                    }

                                                    Label {
                                                        readonly property bool applied:
                                                            precision.opticsEffectState(
                                                                parent.modelData.key).startsWith(
                                                                    qsTr("Applied"))
                                                        text: precision.opticsEffectState(parent.modelData.key)
                                                        color: applied ? Theme.successText : Theme.textMuted
                                                        font.pixelSize: 9
                                                        elide: Text.ElideRight
                                                        visible: text.length > 0
                                                    }

                                                    Switch {
                                                        id: opticsSwitch
                                                        Layout.preferredWidth: 34
                                                        Layout.preferredHeight: 20
                                                        checked: parent.optionChecked
                                                        enabled: precision.editor.active
                                                            && !precision.editor.stateBusy
                                                            && (parent.modelData.key === "master"
                                                                || precision.editor.opticsEnabled)
                                                        onToggled: {
                                                            if (parent.modelData.key === "master")
                                                                precision.editor.opticsEnabled = checked
                                                            else if (parent.modelData.key === "distortion")
                                                                precision.editor.opticsDistortionEnabled = checked
                                                            else if (parent.modelData.key === "tca")
                                                                precision.editor.opticsTcaEnabled = checked
                                                            else if (parent.modelData.key === "vignetting")
                                                                precision.editor.opticsVignettingEnabled = checked
                                                            else
                                                                precision.editor.opticsAutomaticScale = checked
                                                        }
                                                        indicator: Rectangle {
                                                            implicitWidth: 32
                                                            implicitHeight: 16
                                                            x: (opticsSwitch.width - width) / 2
                                                            y: (opticsSwitch.height - height) / 2
                                                            radius: height / 2
                                                            color: opticsSwitch.checked
                                                                ? Theme.switchOnSurface : Theme.switchOffSurface
                                                            border.color: opticsSwitch.checked
                                                                ? Theme.switchOnBorder : Theme.switchOffBorder
                                                            opacity: opticsSwitch.enabled ? 1 : 0.45
                                                            Rectangle {
                                                                width: 10; height: 10; y: 3
                                                                x: opticsSwitch.checked ? parent.width - width - 3 : 3
                                                                radius: width / 2
                                                                color: opticsSwitch.checked ? precision.accent : precision.textMuted
                                                            }
                                                        }
                                                        contentItem: Item {}
                                                    }
                                                }
                                            }
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            Layout.preferredHeight: 1
                                            color: Theme.border
                                        }

                                        Label {
                                            Layout.leftMargin: 14
                                            text: qsTr("DEFRINGE")
                                            color: Theme.textMuted
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                            font.letterSpacing: 0.7
                                        }

                                        ShadowSlider {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Purple amount")
                                            from: 0; to: 1; neutralValue: 0
                                            stepSize: 0.01; decimals: 0
                                            displayMultiplier: 100; suffix: "%"
                                            value: precision.fineValue("defringe_purple_amount")
                                            onGestureStarted: precision.editor.beginParameterEdit(
                                                "defringe_purple_amount")
                                            onEdited: value => precision.editor.setParameterValue(
                                                "defringe_purple_amount", value)
                                            onGestureFinished: precision.editor.endParameterEdit(
                                                "defringe_purple_amount")
                                        }

                                        ShadowHueRange {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Purple hue")
                                            accent: "#b66bd3"
                                            lowerValue: precision.fineValue(
                                                "defringe_purple_hue_low")
                                            upperValue: precision.fineValue(
                                                "defringe_purple_hue_high")
                                            onGestureStarted: precision.editor.beginParameterEdit(
                                                "optics/defringe/purple/hue_range")
                                            onEdited: (lowerValue, upperValue) =>
                                                precision.editor.setDefringeHueRange(
                                                    "purple", lowerValue, upperValue)
                                            onGestureFinished: precision.editor.endParameterEdit(
                                                "optics/defringe/purple/hue_range")
                                        }

                                        ShadowSlider {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Green amount")
                                            from: 0; to: 1; neutralValue: 0
                                            stepSize: 0.01; decimals: 0
                                            displayMultiplier: 100; suffix: "%"
                                            value: precision.fineValue("defringe_green_amount")
                                            onGestureStarted: precision.editor.beginParameterEdit(
                                                "defringe_green_amount")
                                            onEdited: value => precision.editor.setParameterValue(
                                                "defringe_green_amount", value)
                                            onGestureFinished: precision.editor.endParameterEdit(
                                                "defringe_green_amount")
                                        }

                                        ShadowHueRange {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Green hue")
                                            accent: "#58a66b"
                                            lowerValue: precision.fineValue(
                                                "defringe_green_hue_low")
                                            upperValue: precision.fineValue(
                                                "defringe_green_hue_high")
                                            onGestureStarted: precision.editor.beginParameterEdit(
                                                "optics/defringe/green/hue_range")
                                            onEdited: (lowerValue, upperValue) =>
                                                precision.editor.setDefringeHueRange(
                                                    "green", lowerValue, upperValue)
                                            onGestureFinished: precision.editor.endParameterEdit(
                                                "optics/defringe/green/hue_range")
                                        }
                                    }

                                    ShadowAdjustmentSection {
                                        Layout.fillWidth: true
                                        title: qsTr("EFFECTS")

                                        ShadowSlider {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: qsTr("Dehaze")
                                            from: -1; to: 1; neutralValue: 0
                                            stepSize: 0.01; decimals: 0
                                            displayMultiplier: 100; suffix: "%"
                                            value: precision.fineValue("dehaze")
                                            onGestureStarted: precision.editor.beginParameterEdit("dehaze")
                                            onEdited: value => precision.editor.setParameterValue(
                                                "dehaze", value)
                                            onGestureFinished: precision.editor.endParameterEdit("dehaze")
                                        }

                                        Label {
                                            Layout.leftMargin: 14
                                            Layout.topMargin: 4
                                            text: qsTr("GRAIN")
                                            color: Theme.textMuted
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                            font.letterSpacing: 0.7
                                        }

                                        Repeater {
                                            model: [
                                                { "key": "grain_amount", "name": qsTr("Amount"), "neutral": 0 },
                                                { "key": "grain_size", "name": qsTr("Size"), "neutral": 0.5 },
                                                { "key": "grain_roughness", "name": qsTr("Roughness"), "neutral": 0.5 }
                                            ]
                                            delegate: ShadowSlider {
                                                required property var modelData
                                                Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14
                                                label: modelData.name; from: 0; to: 1
                                                neutralValue: modelData.neutral; stepSize: 0.01; decimals: 0
                                                displayMultiplier: 100; suffix: "%"
                                                value: precision.fineValue(modelData.key)
                                                onGestureStarted: precision.editor.beginParameterEdit(modelData.key)
                                                onEdited: value => precision.editor.setParameterValue(modelData.key, value)
                                                onGestureFinished: precision.editor.endParameterEdit(modelData.key)
                                            }
                                        }

                                        Label {
                                            Layout.leftMargin: 14
                                            Layout.topMargin: 4
                                            text: qsTr("POST-CROP VIGNETTE")
                                            color: Theme.textMuted
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                            font.letterSpacing: 0.7
                                        }

                                        Repeater {
                                            model: [
                                                { "key": "vignette_amount", "name": qsTr("Amount"), "from": -1, "neutral": 0 },
                                                { "key": "vignette_midpoint", "name": qsTr("Midpoint"), "from": 0, "neutral": 0.5 },
                                                { "key": "vignette_roundness", "name": qsTr("Roundness"), "from": -1, "neutral": 0 },
                                                { "key": "vignette_feather", "name": qsTr("Feather"), "from": 0, "neutral": 0.5 },
                                                { "key": "vignette_highlights", "name": qsTr("Highlights"), "from": 0, "neutral": 0 }
                                            ]
                                            delegate: ShadowSlider {
                                                required property var modelData
                                                Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14
                                                label: modelData.name; from: modelData.from; to: 1
                                                neutralValue: modelData.neutral; stepSize: 0.01; decimals: 0
                                                displayMultiplier: 100; suffix: "%"
                                                value: precision.fineValue(modelData.key)
                                                onGestureStarted: precision.editor.beginParameterEdit(modelData.key)
                                                onEdited: value => precision.editor.setParameterValue(modelData.key, value)
                                                onGestureFinished: precision.editor.endParameterEdit(modelData.key)
                                            }
                                        }
                                    }

                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.topMargin: 4
                                    spacing: 4

                                    Item { Layout.fillWidth: true }

                                    ShadowIconButton {
                                        id: resetButton
                                        source: "qrc:/icons/redo.svg"
                                        toolTipText: qsTr("Reset the selected Grade Node")
                                        accessibleName: toolTipText
                                        enabled: precision.editor.active
                                            && precision.editor.hasSelectedGradeNode
                                            && !precision.editor.stateBusy
                                        onClicked: precision.editor.resetSelectedGradeNode()
                                    }

                                    ShadowIconButton {
                                        id: revertButton
                                        source: "qrc:/icons/clear.svg"
                                        toolTipText: qsTr("Restore the last autosaved adjustments")
                                        accessibleName: toolTipText
                                        enabled: precision.editor.active
                                            && precision.editor.dirty
                                            && !precision.editor.stateBusy
                                        onClicked: precision.editor.revertEdits()
                                    }
                                }

                                Item { Layout.preferredHeight: 14 }
                            }
                        }
                    }

                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 18
                            spacing: 10

                            Label {
                                text: qsTr("CREATE VERSION CHECKPOINT")
                                color: precision.textMuted
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1.2
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: draftSummary.implicitHeight + 20
                                radius: Theme.controlRadius
                                color: precision.editor.autosaveFailed ? Theme.dangerSurface
                                    : precision.editor.dirty ? Theme.accentSurfaceQuiet
                                    : Theme.panelRaised
                                border.color: precision.editor.autosaveFailed ? Theme.errorBorder
                                    : precision.editor.dirty ? Theme.accentBorder : precision.border

                                ColumnLayout {
                                    id: draftSummary
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 3

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6

                                        Label {
                                            Layout.fillWidth: true
                                            text: !precision.editor.dirty ? qsTr("CURRENT AUTOSAVE")
                                                : precision.editor.autosaveFailed ? qsTr("AUTOSAVE FAILED")
                                                : precision.editor.autosavePending
                                                    ? qsTr("AUTOSAVE PENDING") : qsTr("VERSION DRAFT")
                                            color: !precision.editor.dirty ? precision.textSecondary
                                                : precision.editor.autosaveFailed ? Theme.errorText
                                                : precision.accent
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                            font.letterSpacing: 0.8
                                        }

                                        ShadowIconButton {
                                            visible: precision.editor.autosaveFailed
                                            source: "qrc:/icons/redo.svg"
                                            iconSize: 15
                                            buttonSize: Theme.compactControlHeight
                                            toolTipText: qsTr("Retry autosave")
                                            accessibleName: toolTipText
                                            onClicked: precision.editor.retryAutosave()
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: precision.editor.autosaveFailed
                                            ? precision.editor.autosaveErrorText
                                            : qsTr("Adjustments save automatically to this photo’s current working state. Creating a version adds a named, immutable Library checkpoint; only those checkpoints appear below.")
                                        color: precision.editor.autosaveFailed
                                            ? Theme.errorText : precision.textMuted
                                        font.pixelSize: 9
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            TextField {
                                id: versionLabel
                                Layout.fillWidth: true
                                Layout.preferredHeight: Theme.controlHeight
                                enabled: precision.editor.active && !precision.editor.stateBusy
                                placeholderText: qsTr("Version name")
                                color: precision.textPrimary
                                placeholderTextColor: Theme.textPlaceholder
                                selectByMouse: true
                                background: Rectangle {
                                    radius: Theme.controlRadius
                                    color: Theme.panelRaised
                                    border.color: versionLabel.activeFocus ? precision.accent : precision.border
                                }
                                onAccepted: {
                                    const cleanLabel = text.trim()
                                    if (cleanLabel.length > 0 && saveButton.enabled) {
                                        precision.editor.saveVersion(cleanLabel)
                                        clear()
                                    }
                                }
                            }

                            ShadowButton {
                                id: saveButton
                                Layout.fillWidth: true
                                Layout.preferredHeight: Theme.controlHeight
                                text: precision.editor.stateBusy
                                    ? qsTr("CREATING…") : qsTr("CREATE VERSION")
                                variant: ShadowButton.Primary
                                enabled: precision.editor.active && !precision.editor.stateBusy
                                    && versionLabel.text.trim().length > 0
                                onClicked: {
                                    precision.editor.saveVersion(versionLabel.text.trim())
                                    versionLabel.clear()
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                color: precision.border
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("NAMED VERSIONS")
                                    color: precision.textMuted
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 1.2
                                }
                                Label {
                                    text: qsTr("%L1").arg(versionList.count)
                                    color: precision.textMuted
                                    font.pixelSize: 9
                                }
                            }

                            ListView {
                                id: versionList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                model: precision.editor.versions
                                spacing: 7
                                clip: true

                                delegate: Rectangle {
                                    id: versionRow
                                    required property string commitId
                                    required property string label
                                    required property string createdAtText
                                    required property bool selected
                                    required property int parentCount
                                    required property string changeSummary
                                    required property string parentSummary

                                    width: versionList.width
                                    height: 78
                                    radius: Theme.controlRadius
                                    color: selected
                                        ? Theme.currentRevisionSurface : Theme.panelRaised
                                    border.color: selected
                                        ? Theme.currentRevisionBorder : precision.border

                                    Column {
                                        anchors.left: parent.left
                                        anchors.right: currentBadge.left
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.leftMargin: 11
                                        anchors.rightMargin: 8
                                        spacing: 4
                                        Label {
                                            width: parent.width
                                            text: versionRow.label
                                            color: precision.textPrimary
                                            font.pixelSize: 11
                                            font.weight: Font.Medium
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            width: parent.width
                                            text: versionRow.changeSummary
                                            color: precision.textSecondary
                                            font.pixelSize: 10
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            width: parent.width
                                            text: qsTr("%1  ·  %2")
                                                .arg(versionRow.createdAtText)
                                                .arg(versionRow.parentSummary)
                                            color: precision.textMuted
                                            font.pixelSize: 9
                                            elide: Text.ElideRight
                                        }
                                    }

                                    Label {
                                        id: currentBadge
                                        anchors.right: parent.right
                                        anchors.rightMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: versionRow.selected
                                            ? (precision.editor.versionDraft
                                                ? qsTr("LOADED") : qsTr("CURRENT"))
                                            : qsTr("LOAD")
                                        color: versionRow.selected ? precision.accent : precision.textMuted
                                        font.pixelSize: 8
                                        font.weight: Font.Bold
                                        font.letterSpacing: 0.7
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: !versionRow.selected && !precision.editor.stateBusy
                                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                        onClicked: precision.editor.loadVersionDraft(versionRow.commitId)
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    width: parent.width - 20
                                    visible: versionList.count === 0
                                    text: qsTr("Named checkpoints appear here. The current working adjustments are saved automatically; loading a checkpoint never deletes newer work.")
                                    color: precision.textMuted
                                    font.pixelSize: 10
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                    lineHeight: 1.35
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: recipeRecoveryPopup
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(456, parent.width - 48)
        padding: 0
        modal: true
        dim: true
        closePolicy: Popup.NoAutoClose
        visible: precision.editor.recipeRecoveryRequired

        background: Rectangle {
            radius: Theme.controlRadius + 2
            color: Theme.panelRaised
            border.color: Theme.errorBorder
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: 0

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 22
                spacing: 10

                Label {
                    Layout.fillWidth: true
                    text: qsTr("EDIT RECIPE NEEDS RESET")
                    color: Theme.errorText
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.1
                }

                Label {
                    Layout.fillWidth: true
                    text: precision.editor.recipeRecoveryErrorText
                    color: precision.textPrimary
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                    lineHeight: 1.35
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: precision.border
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 14
                spacing: 8

                ShadowButton {
                    Layout.fillWidth: true
                    text: qsTr("RETURN TO REVIEW")
                    variant: ShadowButton.Secondary
                    enabled: !precision.editor.stateBusy
                    onClicked: precision.returnToReviewRequested()
                }

                ShadowButton {
                    Layout.fillWidth: true
                    text: precision.editor.stateBusy
                        ? qsTr("RESETTING…") : qsTr("RESET EDITS")
                    variant: ShadowButton.Danger
                    enabled: !precision.editor.stateBusy
                    onClicked: precision.editor.resetIncompatibleRecipe()
                }
            }
        }
    }
}
