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
    required property var lutLibrary
    required property var captureMetadata
    required property var displayedHistogram
    required property bool displayingBefore
    required property string readyPreviewGeneration
    required property bool previewFrameReady
    required property bool comparisonActive
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

    property int mixerViewMode: 0
    property int selectedMixerBand: 0
    property int selectedSelectiveColorTarget: 0
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
            Layout.fillWidth: true
            Layout.preferredHeight: 158
            analysis: inspector.displayedHistogram
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
            id: inspectorTabStrip
            Layout.fillWidth: true
            Layout.preferredHeight: 38
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
            currentIndex: 0

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

                        Item { Layout.preferredHeight: 4 }

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

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                title: qsTr("WHITE BALANCE")
                                toolTipText: qsTr("Neutralize the scene before making tonal or creative color adjustments.")

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    spacing: 6
                                    Item { Layout.fillWidth: true }
                                    ShadowIconButton {
                                        source: "qrc:/icons/eyedropper.svg"
                                        selected: inspector.editor.whiteBalancePickerActive
                                        toolTipText: qsTr("Pick a neutral area for White Balance")
                                        accessibleName: toolTipText
                                        onClicked: inspector.editor.setWhiteBalancePickerActive(
                                            !inspector.editor.whiteBalancePickerActive)
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
                                    value: inspector.editor.whiteBalanceTemperature
                                    semanticTrack: true
                                    trackStartColor: "#3979dc"
                                    trackMiddleColor: Theme.track
                                    trackEndColor: "#e49a3a"
                                    onGestureStarted: inspector.editor.beginParameterEdit("white_balance_temperature")
                                    onEdited: value => inspector.editor.whiteBalanceTemperature = value
                                    onGestureFinished: inspector.editor.endParameterEdit("white_balance_temperature")
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
                                    value: inspector.editor.whiteBalanceTint
                                    semanticTrack: true
                                    trackStartColor: "#48a56a"
                                    trackMiddleColor: Theme.track
                                    trackEndColor: "#c65ab4"
                                    onGestureStarted: inspector.editor.beginParameterEdit("white_balance_tint")
                                    onEdited: value => inspector.editor.whiteBalanceTint = value
                                    onGestureFinished: inspector.editor.endParameterEdit("white_balance_tint")
                                }
                            }

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
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
                                    value: inspector.editor.exposureStops
                                    suffix: " EV"
                                    onGestureStarted: inspector.editor.beginParameterEdit("exposure")
                                    onEdited: value => inspector.editor.exposureStops = value
                                    onGestureFinished: inspector.editor.endParameterEdit("exposure")
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
                                    value: inspector.editor.contrastFactor
                                    suffix: "×"
                                    onGestureStarted: inspector.editor.beginParameterEdit("contrast")
                                    onEdited: value => inspector.editor.contrastFactor = value
                                    onGestureFinished: inspector.editor.endParameterEdit("contrast")
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
                                        value: inspector.fineValue(modelData.key)
                                        onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                                        onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
                                    }
                                }
                            }

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
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
                                    value: inspector.editor.saturationFactor
                                    suffix: "×"
                                    onGestureStarted: inspector.editor.beginParameterEdit("saturation")
                                    onEdited: value => inspector.editor.saturationFactor = value
                                    onGestureFinished: inspector.editor.endParameterEdit("saturation")
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
                                    value: inspector.fineValue("vibrance")
                                    onGestureStarted: inspector.editor.beginParameterEdit("vibrance")
                                    onEdited: value => inspector.editor.setParameterValue("vibrance", value)
                                    onGestureFinished: inspector.editor.endParameterEdit("vibrance")
                                }
                            }

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                title: qsTr("CURVE")
                                toolTipText: qsTr("Perceptual lightness curve; hue and chroma are preserved.")

                                ToneCurveEditor {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.preferredHeight: implicitHeight
                                    controller: inspector.editor
                                    panelColor: inspector.panelRaised
                                    plotColor: Theme.chrome
                                    borderColor: inspector.panelBorder
                                    textColor: inspector.textPrimary
                                    mutedTextColor: inspector.textMuted
                                    accentColor: inspector.accent
                                }
                            }

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                title: qsTr("COLOR MIXER")
                                summary: qsTr("OKLCH")
                                toolTipText: qsTr("Adjust the hue, chroma, or Oklab lightness of each color family.")

                                TabBar {
                                    id: mixerViewTabs
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.preferredHeight: 28
                                    background: Rectangle {
                                        radius: Theme.controlRadius
                                        color: Theme.surfaceSubtle
                                        border.color: inspector.panelBorder
                                    }
                                    onCurrentIndexChanged: inspector.mixerViewMode = currentIndex
                                    ShadowTabButton { text: qsTr("OKLCH"); compact: true }
                                    ShadowTabButton { text: qsTr("COLOR"); compact: true }
                                }

                                TabBar {
                                    id: mixerTabs
                                    visible: inspector.mixerViewMode === 0
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
                                    model: inspector.mixerViewMode === 0
                                        ? inspector.colorMixerBands : []
                                    delegate: ShadowSlider {
                                        required property int index
                                        required property var modelData
                                        readonly property string component:
                                            inspector.mixerComponent(mixerTabs.currentIndex)
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 14
                                        Layout.rightMargin: 14
                                        label: modelData.name
                                        accent: modelData.color
                                        semanticTrack: true
                                        trackStartColor: inspector.mixerTrackStart(
                                            modelData, component)
                                        trackMiddleColor: inspector.mixerTrackMiddle(
                                            modelData, component)
                                        trackEndColor: inspector.mixerTrackEnd(
                                            modelData, component)
                                        from: -1.0
                                        to: 1.0
                                        neutralValue: 0.0
                                        stepSize: 0.01
                                        decimals: 0
                                        displayMultiplier: 100
                                        suffix: "%"
                                        value: inspector.mixerValue(index, component)
                                        onGestureStarted: inspector.editor.beginParameterEdit(
                                            "color_mixer/" + component + "/" + index)
                                        onEdited: value => inspector.editor.setColorMixerValue(
                                            index, component, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(
                                            "color_mixer/" + component + "/" + index)
                                    }
                                }

                                RowLayout {
                                    visible: inspector.mixerViewMode === 1
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 20
                                    Layout.rightMargin: 20
                                    Layout.topMargin: visible ? 7 : 0
                                    Layout.bottomMargin: visible ? 5 : 0
                                    Layout.preferredHeight: visible ? 30 : 0
                                    spacing: 8

                                    Item { Layout.fillWidth: true }

                                    Repeater {
                                        model: inspector.colorMixerBands

                                        delegate: Rectangle {
                                            required property int index
                                            required property var modelData
                                            Layout.preferredWidth: 18
                                            Layout.preferredHeight: 18
                                            Layout.alignment: Qt.AlignHCenter
                                            radius: 9
                                            color: modelData.color
                                            border.width: inspector.selectedMixerBand
                                                === index ? 2 : 1
                                            border.color: inspector.selectedMixerBand === index
                                                ? Theme.selectionForeground : Theme.borderStrong
                                            opacity: inspector.selectedMixerBand === index ? 1 : 0.72

                                            TapHandler {
                                                onTapped: inspector.selectedMixerBand = parent.index
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
                                    model: inspector.mixerViewMode === 1 ? [
                                        { "component": "hue", "name": qsTr("Hue") },
                                        { "component": "saturation", "name": qsTr("Chroma") },
                                        { "component": "lightness", "name": qsTr("Lightness") }
                                    ] : []

                                    delegate: ShadowSlider {
                                        required property int index
                                        required property var modelData
                                        readonly property int bandIndex:
                                            inspector.selectedMixerBand
                                        readonly property var band:
                                            inspector.colorMixerBands[bandIndex]
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 14
                                        Layout.rightMargin: 14
                                        Layout.topMargin: index === 0 ? 4 : 0
                                        label: modelData.name
                                        accent: band.color
                                        semanticTrack: true
                                        trackStartColor: inspector.mixerTrackStart(
                                            band, modelData.component)
                                        trackMiddleColor: inspector.mixerTrackMiddle(
                                            band, modelData.component)
                                        trackEndColor: inspector.mixerTrackEnd(
                                            band, modelData.component)
                                        from: -1.0
                                        to: 1.0
                                        neutralValue: 0.0
                                        stepSize: 0.01
                                        decimals: 0
                                        displayMultiplier: 100
                                        suffix: "%"
                                        value: inspector.mixerValue(
                                            bandIndex, modelData.component)
                                        onGestureStarted: inspector.editor.beginParameterEdit(
                                            "color_mixer/" + modelData.component
                                                + "/" + bandIndex)
                                        onEdited: value => inspector.editor.setColorMixerValue(
                                            bandIndex, modelData.component, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(
                                            "color_mixer/" + modelData.component
                                                + "/" + bandIndex)
                                    }
                                }
                            }

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                title: qsTr("SELECTIVE COLOR")
                                summary: qsTr("OKLAB · CMYK")
                                toolTipText: qsTr("Choose an Oklab color family, then apply a Photoshop-style CMYK correction.")

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.topMargin: 3
                                    Layout.bottomMargin: 3
                                    spacing: 4

                                    Item { Layout.fillWidth: true }

                                    Repeater {
                                        model: inspector.selectiveColorTargets

                                        delegate: ShadowColorLabelButton {
                                            required property int index
                                            required property var modelData
                                            buttonSize: 22
                                            labelColor: modelData.color
                                            selected: inspector.selectedSelectiveColorTarget === index
                                            toolTipText: modelData.name
                                            accessibleName: toolTipText
                                            onClicked: inspector.selectedSelectiveColorTarget = index
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
                                    currentIndex: inspector.selectiveColorRelative() ? 0 : 1
                                    background: Rectangle {
                                        radius: Theme.controlRadius
                                        color: Theme.surfaceSubtle
                                        border.color: inspector.panelBorder
                                    }
                                    onCurrentIndexChanged: {
                                        const relative = currentIndex === 0
                                        if (relative === inspector.selectiveColorRelative())
                                            return
                                        inspector.editor.beginParameterEdit("selective_color/method")
                                        inspector.editor.setSelectiveColorRelative(relative)
                                        inspector.editor.endParameterEdit("selective_color/method")
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

                                ShadowSlider {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    Layout.topMargin: 2
                                    label: qsTr("Lightness protection")
                                    toolTipText: qsTr("Preserve the source Oklab lightness after CMYK correction. 0% follows Selective Color; 100% changes hue and chroma only.")
                                    from: 0.0
                                    to: 1.0
                                    neutralValue: 0.0
                                    stepSize: 0.01
                                    decimals: 0
                                    displayMultiplier: 100
                                    suffix: "%"
                                    value: inspector.fineValue(
                                        "selective_color_lightness_protection")
                                    onGestureStarted: inspector.editor.beginParameterEdit(
                                        "selective_color/lightness_protection")
                                    onEdited: value => inspector.editor.setParameterValue(
                                        "selective_color_lightness_protection", value)
                                    onGestureFinished: inspector.editor.endParameterEdit(
                                        "selective_color/lightness_protection")
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
                                            inspector.selectedSelectiveColorTarget
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 14
                                        Layout.rightMargin: 14
                                        label: modelData.name
                                        semanticTrack: true
                                        trackStartColor: inspector.selectiveColorTrackStart(
                                            modelData.component)
                                        trackMiddleColor: Theme.track
                                        trackEndColor: inspector.selectiveColorTrackEnd(
                                            modelData.component)
                                        from: -1.0
                                        to: 1.0
                                        neutralValue: 0.0
                                        stepSize: 0.01
                                        decimals: 0
                                        displayMultiplier: 100
                                        suffix: "%"
                                        value: inspector.selectiveColorValue(
                                            targetIndex, modelData.component)
                                        onGestureStarted: inspector.editor.beginParameterEdit(
                                            "selective_color/" + targetIndex
                                                + "/" + modelData.component)
                                        onEdited: value => inspector.editor.setSelectiveColorValue(
                                            targetIndex, modelData.component, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(
                                            "selective_color/" + targetIndex
                                                + "/" + modelData.component)
                                    }
                                }
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

                            ShadowAdjustmentSection {
                                id: lutSection
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 1
                                title: qsTr("LUT")
                                summary: inspector.editor.hasLut
                                    ? inspector.editor.lutTitle : qsTr("None")
                                toolTipText: qsTr("Apply a managed .cube LUT to this adjustment node. The library button opens LUT management.")

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
                                                        + (inspector.editor.hasLut
                                                            ? inspector.editor.lutResourceId
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
                                                text: inspector.editor.hasLut
                                                    ? inspector.editor.lutTitle
                                                    : qsTr("Choose a LUT")
                                                color: inspector.editor.hasLut
                                                    ? inspector.textPrimary
                                                    : inspector.textMuted
                                                font.pixelSize: 10
                                                elide: Text.ElideRight
                                            }

                                            ShadowIcon {
                                                Layout.preferredWidth: 14
                                                Layout.preferredHeight: 14
                                                size: 14
                                                source: "qrc:/icons/chevron-down.svg"
                                                color: inspector.textSecondary
                                                rotation: lutPicker.opened ? 180 : 0
                                            }
                                        }

                                        MouseArea {
                                            id: lutSelectorMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            enabled: inspector.editor.active
                                                && !inspector.editor.stateBusy
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
                                                    inspector.lutLibrary.availableEntries.length) * 62)
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
                                                model: inspector.lutLibrary.availableEntries

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
                                                            color: inspector.editor.hasLut
                                                                ? inspector.textSecondary
                                                                : inspector.accent
                                                            font.pixelSize: 10
                                                            font.weight: inspector.editor.hasLut
                                                                ? Font.Normal : Font.DemiBold
                                                        }
                                                    }

                                                    MouseArea {
                                                        id: noneLutMouse
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            inspector.editor.clearLut()
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
                                                        inspector.editor.lutResourceId
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
                                                                    ? inspector.accent
                                                                    : inspector.textPrimary
                                                                font.pixelSize: 10
                                                                font.weight: lutOptionRow.current
                                                                    ? Font.DemiBold : Font.Normal
                                                                elide: Text.ElideRight
                                                            }
                                                            Label {
                                                                text: qsTr("%1³").arg(
                                                                    lutOptionRow.modelData.size)
                                                                color: inspector.textMuted
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
                                                            inspector.editor.setLutResource(
                                                                lutOptionRow.modelData.id,
                                                                lutOptionRow.modelData.title,
                                                                lutOptionRow.modelData.managedPath)
                                                            lutPicker.close()
                                                        }
                                                    }
                                                }

                                                Label {
                                                    anchors.centerIn: parent
                                                    visible: inspector.lutLibrary.availableEntries.length === 0
                                                    text: qsTr("No LUTs in the Library")
                                                    color: inspector.textMuted
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
                                        onClicked: inspector.openLutLibraryRequested()
                                    }

                                    ShadowIconButton {
                                        visible: inspector.editor.hasLut
                                        Layout.preferredWidth: visible
                                            ? Theme.controlHeight : 0
                                        Layout.preferredHeight: Theme.controlHeight
                                        Layout.alignment: Qt.AlignVCenter
                                        buttonSize: Theme.controlHeight
                                        variant: ShadowIconButton.Ghost
                                        source: "qrc:/icons/clear.svg"
                                        toolTipText: qsTr("Remove LUT from this Grade Node")
                                        onClicked: inspector.editor.clearLut()
                                    }
                                }

                                ShadowSlider {
                                    visible: inspector.editor.hasLut
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
                                    value: inspector.editor.lutIntensity
                                    onGestureStarted: inspector.editor.beginParameterEdit(
                                        "lut_intensity")
                                    onEdited: value => inspector.editor.lutIntensity = value
                                    onGestureFinished: inspector.editor.endParameterEdit(
                                        "lut_intensity")
                                }

                                RowLayout {
                                    visible: inspector.lutLibrary.availableEntries.length === 0
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("No LUTs in Library")
                                        color: inspector.textMuted
                                        font.pixelSize: 9
                                    }
                                    ShadowIconButton {
                                        buttonSize: 24
                                        iconSize: 16
                                        source: "qrc:/icons/library-manage.svg"
                                        toolTipText: qsTr("Manage LUT Library")
                                        accessibleName: toolTipText
                                        onClicked: inspector.openLutLibraryRequested()
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
                                enabled: inspector.editor.active
                                    && inspector.editor.hasSelectedGradeNode
                                    && !inspector.editor.stateBusy
                                onClicked: inspector.editor.resetSelectedGradeNode()
                            }

                            ShadowIconButton {
                                id: revertButton
                                source: "qrc:/icons/clear.svg"
                                toolTipText: qsTr("Restore the last autosaved adjustments")
                                accessibleName: toolTipText
                                enabled: inspector.editor.active
                                    && inspector.editor.dirty
                                    && !inspector.editor.stateBusy
                                onClicked: inspector.editor.revertEdits()
                            }
                        }

                        Item { Layout.preferredHeight: 14 }
                    }
                }
            }

            PrecisionVersionsPane {
                inspector: inspector
            }
        }
    }
}
