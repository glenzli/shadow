pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: inspector

    // Required context supplied by PrecisionWorkspace.  Keeping this small
    // boundary lets all adjustment controls stay together while the canvas
    // and grade-node list evolve independently.
    required property var editor
    required property int activeToolMode
    required property real cropAspectRatioLock
    required property var lutLibrary
    required property var captureMetadata
    required property var displayedHistogram
    required property bool displayingBefore
    required property string readyPreviewGeneration
    required property bool previewFrameReady
    required property bool comparisonActive
    required property real currentPhotoAspect
    required property real workspaceWidth
    required property color panel
    required property color panelRaised
    required property color panelBorder
    required property color textPrimary
    required property color textSecondary
    required property color textMuted
    required property color accent

    signal openLutLibraryRequested()
    signal openOpticsProfileLibraryRequested()
    signal toolModeRequested(int mode)
    signal cropAspectRatioRequested(real ratio)

    readonly property int toolNone: 0
    readonly property int toolMask: 1
    readonly property int toolCrop: 2
    readonly property int toolRepair: 3

    property bool skinCheckPending: false

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

    function manualOpticsActive() {
        return Number(editor.manualOpticsDistortion) !== 0
            || Number(editor.manualOpticsTcaRedCyan) !== 0
            || Number(editor.manualOpticsTcaBlueYellow) !== 0
            || Number(editor.manualOpticsVignettingAmount) !== 0
    }

    function fineValue(key) {
        // Reading the revision makes generic key lookups reactive without
        // exposing dozens of one-off Q_PROPERTY accessors.
        const revision = editor.parameterRevision
        return revision >= 0 ? editor.parameterValue(key) : 0
    }

    Connections {
        target: inspector.editor

        function onParametersChanged() {
            if (!inspector.skinCheckPending
                    || inspector.editor.selectedPointColorIndex < 0)
                return
            inspector.skinCheckPending = false
            analysisScope.scopeMode = analysisScope.vectorscopeScope
            analysisScope.skinGuideVisible = true
            inspector.editor.pointColorScopeActive = true
        }

        function onPointColorPickerActiveChanged() {
            if (inspector.skinCheckPending
                    && !inspector.editor.pointColorPickerActive
                    && inspector.editor.selectedPointColorIndex < 0) {
                inspector.skinCheckPending = false
            }
        }

        function onSelectedGradeNodeChanged() {
            inspector.skinCheckPending = false
        }
    }

    Layout.preferredWidth: Math.max(304, Math.min(348, inspector.workspaceWidth * 0.24))
    Layout.fillHeight: true
    color: inspector.panel
    border.width: 0

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: 1
        color: inspector.panelBorder
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        EditHistogram {
            id: analysisScope
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            analysis: inspector.displayedHistogram
            editor: inspector.editor
            beforeView: inspector.displayingBefore
            displayGeneration: inspector.readyPreviewGeneration
            panelColor: inspector.panel
            plotColor: Theme.plot
            borderColor: Theme.transparent
            textColor: inspector.textPrimary
            secondaryTextColor: inspector.textSecondary
            mutedTextColor: inspector.textMuted
            accentColor: inspector.accent
        }

        Rectangle {
            id: captureMetadataPanel
            Layout.fillWidth: true
            Layout.preferredHeight: 58
            color: inspector.panel

            readonly property bool metadataMatches:
                inspector.captureMetadata
                && inspector.captureMetadata.representationId
                    === inspector.editor.representationId
            readonly property bool metadataAvailable:
                metadataMatches && inspector.captureMetadata.available

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: inspector.panelBorder
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
                        ? inspector.joinedIdentity(
                            inspector.captureMetadata.cameraMake,
                            inspector.captureMetadata.cameraModel)
                        : captureMetadataPanel.metadataMatches
                            && inspector.captureMetadata.pending
                            ? qsTr("Preparing capture metadata…")
                            : qsTr("Capture metadata unavailable")
                    color: captureMetadataPanel.metadataAvailable
                        ? inspector.textPrimary : inspector.textMuted
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
                            ? inspector.captureSettingSummary() : ""
                        color: inspector.textSecondary
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.maximumWidth: parent.width * 0.42
                        visible: captureMetadataPanel.metadataAvailable
                            && text.length > 0
                        text: inspector.joinedIdentity(
                            inspector.captureMetadata.lensMake,
                            inspector.captureMetadata.lensModel)
                        color: inspector.textMuted
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Rectangle {
            id: specialToolStrip
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            color: inspector.panel

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: inspector.panelBorder
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 4

                ShadowIconButton {
                    source: "qrc:/icons/mask.svg"
                    selected: inspector.activeToolMode === inspector.toolMask
                    toolTipText: qsTr("Mask")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active
                        && inspector.editor.hasSelectedGradeNode
                        && !inspector.editor.stateBusy
                    onClicked: inspector.toolModeRequested(inspector.toolMask)
                }

                ShadowIconButton {
                    source: "qrc:/icons/crop.svg"
                    selected: inspector.activeToolMode === inspector.toolCrop
                    toolTipText: qsTr("Crop and straighten")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active
                        && inspector.previewFrameReady
                        && !inspector.editor.stateBusy
                    onClicked: inspector.toolModeRequested(inspector.toolCrop)
                }

                ShadowIconButton {
                    source: "qrc:/icons/retouch.svg"
                    selected: inspector.activeToolMode === inspector.toolRepair
                    toolTipText: qsTr("Repair")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active
                        && inspector.previewFrameReady
                        && !inspector.editor.stateBusy
                    onClicked: inspector.toolModeRequested(inspector.toolRepair)
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 20
                    color: inspector.panelBorder
                }

                ShadowIconButton {
                    source: "qrc:/icons/clear.svg"
                    toolTipText: qsTr("Reset all adjustments · Undo available")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active
                        && !inspector.editor.stateBusy
                    onClicked: inspector.editor.resetAllAdjustments()
                }
            }
        }

        Rectangle {
            id: inspectorTabStrip
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 38 : 0
            visible: inspector.activeToolMode === inspector.toolNone
            color: inspector.panel

            property int currentIndex: 0

            function selectTab(index) {
                if (currentIndex === index)
                    return
                currentIndex = index
                Qt.callLater(function() {
                    if (inspectorScroll.contentItem)
                        inspectorScroll.contentItem.contentY = 0
                })
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: inspector.panelBorder
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 0

                ShadowTabButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: inspectorTabStrip.currentIndex === 0
                    minimumTabWidth: 92
                    underlineInset: 28
                    text: qsTr("ADJUST")
                    toolTipText: qsTr("Core tone, color, detail, and optics controls")
                    onClicked: inspectorTabStrip.selectTab(0)
                }

                ShadowTabButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: inspectorTabStrip.currentIndex === 1
                    minimumTabWidth: 92
                    underlineInset: 28
                    text: qsTr("LOOKS")
                    toolTipText: qsTr("Color grading, LUTs, and finishing effects")
                    onClicked: inspectorTabStrip.selectTab(1)
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Photo working state is always autosaved.  The old
            // per-photo checkpoint page is deliberately kept out of
            // Precision while versioning moves to the catalog-level
            // History workspace; it must not masquerade as a global
            // Git-like commit view here.
            currentIndex: inspector.activeToolMode === inspector.toolNone ? 0 : 1

            Item {
                ScrollView {
                    id: inspectorScroll
                    anchors.fill: parent
                    clip: true
                    contentWidth: availableWidth

                    ColumnLayout {
                        width: parent.width
                        spacing: 7
                        enabled: inspector.editor.active && !inspector.editor.stateBusy

                        Item { Layout.preferredHeight: 8 }

                        ColumnLayout {
                            objectName: "gradeNodeInspector"
                            Layout.fillWidth: true
                            spacing: 8
                            enabled: inspector.editor.gradeNodeEnabled
                            opacity: inspector.editor.gradeNodeEnabled
                                ? 1.0 : 0.42

                            Behavior on opacity {
                                NumberAnimation { duration: 100 }
                            }

                            PrecisionFoundationAdjustments {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                editor: inspector.editor
                                panelRaised: inspector.panelRaised
                                panelBorder: inspector.panelBorder
                                textPrimary: inspector.textPrimary
                                textMuted: inspector.textMuted
                                accent: inspector.accent
                            }

                            PrecisionColorMixer {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                editor: inspector.editor
                                panelRaised: inspector.panelRaised
                                panelBorder: inspector.panelBorder
                                textPrimary: inspector.textPrimary
                                textMuted: inspector.textMuted
                                accent: inspector.accent
                            }

                            PrecisionSelectiveColor {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                editor: inspector.editor
                                panelBorder: inspector.panelBorder
                            }

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                title: qsTr("POINT COLOR")
                                toolTipText: qsTr("Use the eyedropper to build one or more precise Oklch color ranges from the image.")

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 20
                                    Layout.rightMargin: 20
                                    Layout.topMargin: 5
                                    Layout.bottomMargin: 6
                                    spacing: 8

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 5

                                        Repeater {
                                            model: inspector.editor.pointColors

                                            delegate: Rectangle {
                                                required property var modelData
                                                Layout.preferredWidth: 20
                                                Layout.preferredHeight: 20
                                                radius: 10
                                                color: modelData.swatch
                                                border.width: inspector.editor.selectedPointColorIndex
                                                    === modelData.index ? 2 : 1
                                                border.color: inspector.editor.selectedPointColorIndex
                                                    === modelData.index
                                                    ? inspector.accent : Theme.borderStrong

                                                TapHandler {
                                                    onTapped: inspector.editor.selectPointColor(
                                                        parent.modelData.index)
                                                }
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    ShadowIconButton {
                                        source: "qrc:/icons/eyedropper.svg"
                                        selected: inspector.editor.pointColorPickerActive
                                        enabled: inspector.editor.active
                                            && inspector.editor.pointColors.length < 16
                                            && inspector.previewFrameReady
                                            && inspector.readyPreviewGeneration.length > 0
                                            && !inspector.comparisonActive
                                        toolTipText: qsTr("Add a Point Color sample from the image")
                                        accessibleName: toolTipText
                                        onClicked: inspector.editor.setPointColorPickerActive(
                                            !inspector.editor.pointColorPickerActive)
                                    }

                                    ShadowIconButton {
                                        source: "qrc:/icons/trash.svg"
                                        variant: ShadowIconButton.Danger
                                        enabled: inspector.editor.selectedPointColorIndex >= 0
                                        toolTipText: qsTr("Remove selected Point Color sample")
                                        accessibleName: toolTipText
                                        onClicked: inspector.editor.removeSelectedPointColor()
                                    }
                                }

                                // A skin check is a contextual diagnostic, not a primary action.
                                // Keep its short label (there is no universally readable "skin" icon),
                                // but avoid giving it a full inspector-width button treatment.
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.topMargin: 2
                                    Layout.bottomMargin: 2
                                    spacing: 5

                                    ShadowIconButton {
                                        buttonSize: 24
                                        iconSize: 15
                                        source: "qrc:/icons/scopes.svg"
                                        selected: inspector.editor.pointColorScopeActive
                                        toolTipText: qsTr("Sample a representative skin midtone, freeze its diagnostic pixels, and inspect shadow, midtone, and highlight alignment in the Vectorscope.")
                                        accessibleName: toolTipText
                                        onClicked: skinCheckAction.clicked()
                                    }

                                    ShadowButton {
                                        id: skinCheckAction
                                        compact: true
                                        minimumButtonWidth: 0
                                        variant: selected ? ShadowButton.Tinted : ShadowButton.Ghost
                                        selected: inspector.editor.pointColorScopeActive
                                        text: selected
                                            ? qsTr("SKIN REFERENCE LOCKED")
                                            : qsTr("SKIN CHECK")
                                        toolTipText: qsTr("Sample a representative skin midtone, freeze its diagnostic pixels, and inspect shadow, midtone, and highlight alignment in the Vectorscope.")
                                        onClicked: {
                                            analysisScope.scopeMode = analysisScope.vectorscopeScope
                                            analysisScope.skinGuideVisible = true
                                            if (inspector.editor.pointColorScopeActive) {
                                                inspector.skinCheckPending = false
                                                inspector.editor.pointColorScopeActive = false
                                            } else if (inspector.editor.pointColorScopeAvailable) {
                                                inspector.skinCheckPending = false
                                                inspector.editor.pointColorScopeActive = true
                                            } else {
                                                inspector.skinCheckPending = true
                                                inspector.editor.setPointColorPickerActive(true)
                                            }
                                        }
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                Rectangle {
                                    id: skinGuideNudge
                                    // A single Point Color adjustment is appropriate only when the
                                    // selected skin is both representative and tonally coherent.
                                    // The guide itself stays diagnostic; these guards prevent a
                                    // low-sample or split-tone reading from becoming a broad fix.
                                    readonly property int minimumMatchedPixels: 96
                                    readonly property real maximumToneDeviationSpread: 15
                                    readonly property real guideDeviation:
                                        analysisScope.displayScopeSkinGuideDeviation
                                    readonly property var shadows:
                                        analysisScope.skinToneRange("Shadows")
                                    readonly property var midtones:
                                        analysisScope.skinToneRange("Midtones")
                                    readonly property var highlights:
                                        analysisScope.skinToneRange("Highlights")
                                    readonly property real toneDeviationSpread: {
                                        const ranges = [shadows, midtones, highlights]
                                        let largest = 0
                                        for (let first = 0; first < ranges.length; ++first) {
                                            if (!ranges[first].available)
                                                continue
                                            for (let second = first + 1; second < ranges.length; ++second) {
                                                if (!ranges[second].available)
                                                    continue
                                                const wrapped = (ranges[first].deviation
                                                    - ranges[second].deviation + 540) % 360 - 180
                                                largest = Math.max(largest, Math.abs(wrapped))
                                            }
                                        }
                                        return largest
                                    }
                                    readonly property bool hasSufficientSample:
                                        analysisScope.displayScopeMatchedPixels >= minimumMatchedPixels
                                    readonly property bool toneSplit:
                                        toneDeviationSpread > maximumToneDeviationSpread
                                    readonly property real requestedHueNudge: Math.max(-12, Math.min(
                                        12, -guideDeviation))
                                    readonly property int roundedGuideDeviation: Math.round(guideDeviation)
                                    readonly property int roundedHueNudge: Math.round(requestedHueNudge)
                                    readonly property bool nudgeAvailable: hasSufficientSample
                                        && !toneSplit
                                        && Math.abs(requestedHueNudge) >= 0.5
                                    visible: inspector.editor.pointColorScopeActive
                                        && analysisScope.displayScopeCentroidAvailable
                                        && inspector.editor.selectedPointColorIndex >= 0
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.bottomMargin: visible ? 5 : 0
                                    Layout.preferredHeight: visible ? 31 : 0
                                    radius: 4
                                    color: Qt.rgba(0.92, 0.55, 0.37, 0.09)
                                    border.width: 1
                                    border.color: Qt.rgba(0.92, 0.55, 0.37, 0.35)

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 8
                                        anchors.rightMargin: 5
                                        spacing: 6

                                        Label {
                                            Layout.fillWidth: true
                                            text: skinGuideNudge.toneSplit
                                                ? qsTr("TONE SPLIT · USE SEPARATE NODES")
                                                : !skinGuideNudge.hasSufficientSample
                                                    ? qsTr("SAMPLE TOO SMALL · REFINE POINT COLOR")
                                                    : !skinGuideNudge.nudgeAvailable
                                                        ? qsTr("ALIGNED · NO NUDGE NEEDED")
                                                        : qsTr("SKIN GUIDE Δ %1").arg(
                                                            (skinGuideNudge.roundedGuideDeviation > 0
                                                                ? "+" : "")
                                                            + skinGuideNudge.roundedGuideDeviation + "°")
                                            color: skinGuideNudge.nudgeAvailable
                                                ? Theme.textSecondary : "#e6a36c"
                                            font.pixelSize: 10
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }

                                        RowLayout {
                                            visible: skinGuideNudge.nudgeAvailable
                                            spacing: 2

                                            Label {
                                                text: (skinGuideNudge.roundedHueNudge > 0 ? "+" : "")
                                                    + skinGuideNudge.roundedHueNudge + "°"
                                                color: Theme.textSecondary
                                                font.pixelSize: 10
                                                font.weight: Font.DemiBold
                                            }

                                            ShadowIconButton {
                                                buttonSize: 24
                                                iconSize: 14
                                                source: "qrc:/icons/edit.svg"
                                                enabled: !inspector.editor.stateBusy
                                                toolTipText: qsTr("Apply the guide direction as a limited starting hue correction for this Point Color. It is undoable and does not change the node mask or global color.")
                                                accessibleName: toolTipText
                                                onClicked: {
                                                    const current = inspector.fineValue("color_range_hue")
                                                    const next = Math.max(-180, Math.min(180,
                                                        current + skinGuideNudge.requestedHueNudge))
                                                    if (next === current)
                                                        return
                                                    inspector.editor.beginParameterEdit(
                                                        "skin_guide/point_color_hue")
                                                    inspector.editor.setParameterValue(
                                                        "color_range_hue", next)
                                                    inspector.editor.endParameterEdit(
                                                        "skin_guide/point_color_hue")
                                                }
                                            }
                                        }
                                    }
                                }

                                Rectangle {
                                    visible: inspector.editor.selectedPointColorIndex >= 0
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
                                            ((inspector.fineValue("color_range_center")
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
                                        visible: inspector.editor.selectedPointColorIndex >= 0
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
                                        value: inspector.fineValue(modelData.key)
                                        enabled: inspector.editor.selectedPointColorIndex >= 0
                                        onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                                        onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
                                    }
                                }
                            }

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                title: qsTr("COLOR MAP")
                                summary: qsTr("OKLAB 5×5")
                                toolTipText: qsTr("Move a smooth connected Oklab mesh after Color Mixer and Point Color. This is a separate chroma-field correction, not a hue-keyed slider.")

                                ColorWarperEditor {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.bottomMargin: 3
                                    controller: inspector.editor
                                }
                            }

                            PrecisionLutSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 1
                                editor: inspector.editor
                                lutLibrary: inspector.lutLibrary
                                textPrimary: inspector.textPrimary
                                textSecondary: inspector.textSecondary
                                textMuted: inspector.textMuted
                                accent: inspector.accent
                                onOpenLibraryRequested:
                                    inspector.openLutLibraryRequested()
                            }

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 1
                                title: qsTr("COLOR GRADING")
                                toolTipText: qsTr("Tint shadows, midtones, and highlights independently with perceptual color wheels.")

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
                                            hue: inspector.fineValue(modelData.hue)
                                            saturation: inspector.fineValue(modelData.saturation)
                                            luminance: inspector.fineValue(modelData.luminance)
                                            onWheelGestureStarted: inspector.editor.beginParameterEdit(
                                                "color_grading/" + modelData.range + "/wheel")
                                            onWheelEdited: (hue, saturation) =>
                                                inspector.editor.setColorGradingWheel(
                                                    modelData.range, hue, saturation)
                                            onWheelGestureFinished: inspector.editor.endParameterEdit(
                                                "color_grading/" + modelData.range + "/wheel")
                                            onLuminanceGestureStarted: inspector.editor.beginParameterEdit(
                                                modelData.luminance)
                                            onLuminanceEdited: value =>
                                                inspector.editor.setParameterValue(
                                                    modelData.luminance, value)
                                            onLuminanceGestureFinished: inspector.editor.endParameterEdit(
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
                                        value: inspector.fineValue(modelData.key)
                                        onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                                        onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
                                    }
                                }
                            }

                            PrecisionTechnicalTools {
                                Layout.fillWidth: true
                                inspector: inspector
                                currentTabIndex: inspectorTabStrip.currentIndex
                                onOpenOpticsProfileLibraryRequested:
                                    inspector.openOpticsProfileLibraryRequested()
                            }

                        }

                        Item { Layout.preferredHeight: 14 }
                    }
                }
            }

            Item {
                ScrollView {
                    id: specialToolScroll
                    anchors.fill: parent
                    clip: true
                    contentWidth: availableWidth

                    ColumnLayout {
                        width: parent.width
                        spacing: 0
                        enabled: inspector.editor.active
                            && !inspector.editor.stateBusy

                        PrecisionLocalMaskTools {
                            Layout.fillWidth: true
                            visible: inspector.activeToolMode === inspector.toolMask
                            inspector: inspector
                            currentTabIndex: 0
                        }

                        PrecisionGeometryTools {
                            Layout.fillWidth: true
                            visible: inspector.activeToolMode === inspector.toolCrop
                            inspector: inspector
                            currentTabIndex: 0
                            aspectRatioLock: inspector.cropAspectRatioLock
                            onAspectRatioRequested: ratio =>
                                inspector.cropAspectRatioRequested(ratio)
                        }

                        PrecisionRetouchTools {
                            Layout.fillWidth: true
                            visible: inspector.activeToolMode === inspector.toolRepair
                            inspector: inspector
                            currentTabIndex: 0
                        }

                        Item { Layout.preferredHeight: 14 }
                    }
                }
            }
        }
    }
}
