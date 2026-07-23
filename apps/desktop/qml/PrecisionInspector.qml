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

                            Label {
                                Layout.fillWidth: true
                                Layout.leftMargin: 14
                                Layout.rightMargin: 14
                                text: inspector.editor.hasSelectedGradeNode
                                    ? inspector.editor.gradeNodes[
                                        inspector.editor
                                            .selectedGradeNodeIndex].label
                                    : qsTr("No Grade Node selected")
                                color: inspector.textPrimary
                                font.pixelSize: 16
                                font.weight: Font.Medium
                                elide: Text.ElideRight
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
                                title: qsTr("TONE CURVE")

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

                                        Label {
                                            visible: inspector.editor.pointColors.length === 0
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

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 0
                                title: qsTr("DETAIL")
                                toolTipText: qsTr("Control perceptual frequency detail, capture sharpening, and conventional noise reduction.")

                                ShadowSubsectionLabel {
                                    text: qsTr("FREQUENCY DETAIL")
                                    toolTipText: qsTr("Clarity changes protected mid-frequency structure; Texture changes the smaller residual. Both operate only on Oklab L.")
                                }

                                Repeater {
                                    model: [
                                        { "key": "clarity", "name": qsTr("Clarity") },
                                        { "key": "texture", "name": qsTr("Texture") }
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
                                        onGestureStarted: inspector.editor.beginParameterEdit(
                                            modelData.key)
                                        onEdited: value => inspector.editor.setParameterValue(
                                            modelData.key, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(
                                            modelData.key)
                                    }
                                }

                                ShadowSubsectionLabel {
                                    Layout.topMargin: 6
                                    text: qsTr("SHARPENING")
                                    toolTipText: qsTr("Conventional capture sharpening. Use after frequency detail, and mask it to avoid sharpening smooth noise.")
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
                                        value: inspector.fineValue(modelData.key)
                                        onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                                        onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
                                    }
                                }

                                ShadowSubsectionLabel {
                                    Layout.topMargin: 6
                                    text: qsTr("DENOISE")
                                    toolTipText: qsTr("Conventional RGB preview denoise. RAW-domain denoise is planned separately in the RAW development pipeline.")
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
                                title: qsTr("OPTICS")

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    spacing: 5

                                    ShadowSubsectionLabel {
                                        text: qsTr("PROFILE CORRECTION")
                                        toolTipText: qsTr("Lensfun supplies a calibrated baseline when a compatible camera and lens profile is available.")
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        Label {
                                            Layout.fillWidth: true
                                            text: {
                                            const receipt = inspector.editor.opticsReceipt
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
                                            color: inspector.editor.opticsReceipt.status === "matched"
                                                ? Theme.successText : Theme.textMuted
                                            font.pixelSize: 10
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            visible: inspector.manualOpticsActive()
                                            text: qsTr("Manual residual active")
                                            color: inspector.accent
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }
                                        ShadowIconButton {
                                            source: "qrc:/icons/library-manage.svg"
                                            buttonSize: 26
                                            toolTipText: qsTr("Choose optical profile")
                                            accessibleName: toolTipText
                                            onClicked: inspector.openOpticsProfileLibraryRequested()
                                        }
                                    }

                                    Repeater {
                                        model: [
                                            { "key": "master", "name": qsTr("Profile correction") },
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
                                                modelData.key === "master" ? inspector.editor.opticsEnabled
                                                : modelData.key === "distortion" ? inspector.editor.opticsDistortionEnabled
                                                : modelData.key === "tca" ? inspector.editor.opticsTcaEnabled
                                                : modelData.key === "vignetting" ? inspector.editor.opticsVignettingEnabled
                                                : inspector.editor.opticsAutomaticScale

                                            Label {
                                                Layout.fillWidth: true
                                                text: parent.modelData.name
                                                color: parent.enabled ? Theme.textSecondary : Theme.textDisabled
                                                font.pixelSize: 10
                                            }

                                            Label {
                                                readonly property bool applied:
                                                    inspector.opticsEffectState(
                                                        parent.modelData.key).startsWith(
                                                            qsTr("Applied"))
                                                text: inspector.opticsEffectState(parent.modelData.key)
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
                                                enabled: inspector.editor.active
                                                    && !inspector.editor.stateBusy
                                                    && (parent.modelData.key === "master"
                                                        || inspector.editor.opticsEnabled)
                                                onToggled: {
                                                    if (parent.modelData.key === "master")
                                                        inspector.editor.opticsEnabled = checked
                                                    else if (parent.modelData.key === "distortion")
                                                        inspector.editor.opticsDistortionEnabled = checked
                                                    else if (parent.modelData.key === "tca")
                                                        inspector.editor.opticsTcaEnabled = checked
                                                    else if (parent.modelData.key === "vignetting")
                                                        inspector.editor.opticsVignettingEnabled = checked
                                                    else
                                                        inspector.editor.opticsAutomaticScale = checked
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
                                                        color: opticsSwitch.checked ? inspector.accent : inspector.textMuted
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

                                ShadowSubsectionLabel {
                                    Layout.topMargin: 4
                                    text: qsTr("MANUAL OPTICS")
                                    toolTipText: qsTr("Profile-independent residual correction. These controls stay available for manual lenses or images without a matching profile.")
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    text: qsTr("Applied after the selected Profile correction; works without a profile.")
                                    color: inspector.textMuted
                                    font.pixelSize: 9
                                    wrapMode: Text.WordWrap
                                }

                                ShadowSlider {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    label: qsTr("Distortion")
                                    toolTipText: qsTr("Residual radial geometry correction. Use after the Profile when straight lines still bow.")
                                    from: -100; to: 100; neutralValue: 0
                                    stepSize: 1; decimals: 0; suffix: "%"
                                    value: inspector.editor.manualOpticsDistortion
                                    onEdited: value => inspector.editor.manualOpticsDistortion = Math.round(value)
                                }

                                ShadowSubsectionLabel {
                                    Layout.topMargin: 5
                                    text: qsTr("LATERAL CHROMATIC ABERRATION")
                                    toolTipText: qsTr("Geometrically realign color channels. This is distinct from purple/green Defringe below.")
                                }

                                ShadowSlider {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    label: qsTr("Red / Cyan")
                                    toolTipText: qsTr("Move the red channel radially against green to correct red/cyan color fringes.")
                                    from: -100; to: 100; neutralValue: 0
                                    stepSize: 1; decimals: 0; suffix: "%"
                                    value: inspector.editor.manualOpticsTcaRedCyan
                                    onEdited: value => inspector.editor.manualOpticsTcaRedCyan = Math.round(value)
                                }

                                ShadowSlider {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    label: qsTr("Blue / Yellow")
                                    toolTipText: qsTr("Move the blue channel radially against green to correct blue/yellow color fringes.")
                                    from: -100; to: 100; neutralValue: 0
                                    stepSize: 1; decimals: 0; suffix: "%"
                                    value: inspector.editor.manualOpticsTcaBlueYellow
                                    onEdited: value => inspector.editor.manualOpticsTcaBlueYellow = Math.round(value)
                                }

                                ShadowSubsectionLabel {
                                    Layout.topMargin: 5
                                    text: qsTr("OPTICAL VIGNETTING")
                                    toolTipText: qsTr("Lens shading correction in original optical coordinates. The creative post-crop vignette is in Looks.")
                                }

                                ShadowSlider {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    label: qsTr("Amount")
                                    toolTipText: qsTr("Brighten or darken the outer lens shading before crop and creative grading.")
                                    from: -100; to: 100; neutralValue: 0
                                    stepSize: 1; decimals: 0; suffix: "%"
                                    value: inspector.editor.manualOpticsVignettingAmount
                                    onEdited: value => inspector.editor.manualOpticsVignettingAmount = Math.round(value)
                                }

                                ShadowSlider {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    label: qsTr("Midpoint")
                                    from: 0; to: 100; neutralValue: 50
                                    stepSize: 1; decimals: 0; suffix: "%"
                                    value: inspector.editor.manualOpticsVignettingMidpoint
                                    onEdited: value => inspector.editor.manualOpticsVignettingMidpoint = Math.round(value)
                                }

                                ShadowSubsectionLabel {
                                    Layout.topMargin: 4
                                    text: qsTr("DEFRINGE")
                                    toolTipText: qsTr("Suppress purple and green chromatic fringes within the selected hue ranges.")
                                }

                                ShadowSlider {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    label: qsTr("Purple amount")
                                    from: 0; to: 1; neutralValue: 0
                                    stepSize: 0.01; decimals: 0
                                    displayMultiplier: 100; suffix: "%"
                                    value: inspector.fineValue("defringe_purple_amount")
                                    onGestureStarted: inspector.editor.beginParameterEdit(
                                        "defringe_purple_amount")
                                    onEdited: value => inspector.editor.setParameterValue(
                                        "defringe_purple_amount", value)
                                    onGestureFinished: inspector.editor.endParameterEdit(
                                        "defringe_purple_amount")
                                }

                                ShadowHueRange {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    label: qsTr("Purple hue")
                                    accent: "#b66bd3"
                                    lowerValue: inspector.fineValue(
                                        "defringe_purple_hue_low")
                                    upperValue: inspector.fineValue(
                                        "defringe_purple_hue_high")
                                    onGestureStarted: inspector.editor.beginParameterEdit(
                                        "optics/defringe/purple/hue_range")
                                    onEdited: (lowerValue, upperValue) =>
                                        inspector.editor.setDefringeHueRange(
                                            "purple", lowerValue, upperValue)
                                    onGestureFinished: inspector.editor.endParameterEdit(
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
                                    value: inspector.fineValue("defringe_green_amount")
                                    onGestureStarted: inspector.editor.beginParameterEdit(
                                        "defringe_green_amount")
                                    onEdited: value => inspector.editor.setParameterValue(
                                        "defringe_green_amount", value)
                                    onGestureFinished: inspector.editor.endParameterEdit(
                                        "defringe_green_amount")
                                }

                                ShadowHueRange {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    label: qsTr("Green hue")
                                    accent: "#58a66b"
                                    lowerValue: inspector.fineValue(
                                        "defringe_green_hue_low")
                                    upperValue: inspector.fineValue(
                                        "defringe_green_hue_high")
                                    onGestureStarted: inspector.editor.beginParameterEdit(
                                        "optics/defringe/green/hue_range")
                                    onEdited: (lowerValue, upperValue) =>
                                        inspector.editor.setDefringeHueRange(
                                            "green", lowerValue, upperValue)
                                    onGestureFinished: inspector.editor.endParameterEdit(
                                        "optics/defringe/green/hue_range")
                                }
                            }

                            ShadowAdjustmentSection {
                                Layout.fillWidth: true
                                visible: inspectorTabStrip.currentIndex === 1
                                title: qsTr("EFFECTS")

                                ShadowSlider {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 14
                                    label: qsTr("Dehaze")
                                    from: -1; to: 1; neutralValue: 0
                                    stepSize: 0.01; decimals: 0
                                    displayMultiplier: 100; suffix: "%"
                                    value: inspector.fineValue("dehaze")
                                    onGestureStarted: inspector.editor.beginParameterEdit("dehaze")
                                    onEdited: value => inspector.editor.setParameterValue(
                                        "dehaze", value)
                                    onGestureFinished: inspector.editor.endParameterEdit("dehaze")
                                }

                                ShadowSubsectionLabel {
                                    Layout.topMargin: 6
                                    text: qsTr("GRAIN")
                                    toolTipText: qsTr("Add a controlled photographic grain after the main color and tone adjustments.")
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
                                        value: inspector.fineValue(modelData.key)
                                        onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                                        onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
                                    }
                                }

                                ShadowSubsectionLabel {
                                    Layout.topMargin: 6
                                    text: qsTr("POST-CROP VIGNETTE")
                                    toolTipText: qsTr("Apply a creative vignette after cropping; this is separate from optical lens-vignetting correction.")
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
                                        value: inspector.fineValue(modelData.key)
                                        onGestureStarted: inspector.editor.beginParameterEdit(modelData.key)
                                        onEdited: value => inspector.editor.setParameterValue(modelData.key, value)
                                        onGestureFinished: inspector.editor.endParameterEdit(modelData.key)
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

            Item {
                // Transitional implementation retained for the future
                // History/checkpoint migration. It has no entry point
                // in Precision.
                visible: false
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 10

                    Label {
                        text: qsTr("CREATE VERSION CHECKPOINT")
                        color: inspector.textMuted
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.2
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: draftSummary.implicitHeight + 20
                        radius: Theme.controlRadius
                        color: inspector.editor.autosaveFailed ? Theme.dangerSurface
                            : inspector.editor.dirty ? Theme.accentSurfaceQuiet
                            : Theme.panelRaised
                        border.color: inspector.editor.autosaveFailed ? Theme.errorBorder
                            : inspector.editor.dirty ? Theme.accentBorder : inspector.panelBorder

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
                                    text: !inspector.editor.dirty ? qsTr("CURRENT AUTOSAVE")
                                        : inspector.editor.autosaveFailed ? qsTr("AUTOSAVE FAILED")
                                        : inspector.editor.autosavePending
                                            ? qsTr("AUTOSAVE PENDING") : qsTr("VERSION DRAFT")
                                    color: !inspector.editor.dirty ? inspector.textSecondary
                                        : inspector.editor.autosaveFailed ? Theme.errorText
                                        : inspector.accent
                                    font.pixelSize: 9
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 0.8
                                }

                                ShadowIconButton {
                                    visible: inspector.editor.autosaveFailed
                                    source: "qrc:/icons/redo.svg"
                                    iconSize: 15
                                    buttonSize: Theme.compactControlHeight
                                    toolTipText: qsTr("Retry autosave")
                                    accessibleName: toolTipText
                                    onClicked: inspector.editor.retryAutosave()
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: inspector.editor.autosaveFailed
                                    ? inspector.editor.autosaveErrorText
                                    : qsTr("Adjustments save automatically to this photo’s current working state. Creating a version adds a named, immutable Library checkpoint; only those checkpoints appear below.")
                                color: inspector.editor.autosaveFailed
                                    ? Theme.errorText : inspector.textMuted
                                font.pixelSize: 9
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    TextField {
                        id: versionLabel
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.controlHeight
                        enabled: inspector.editor.active && !inspector.editor.stateBusy
                        placeholderText: qsTr("Version name")
                        color: inspector.textPrimary
                        placeholderTextColor: Theme.textPlaceholder
                        selectByMouse: true
                        background: Rectangle {
                            radius: Theme.controlRadius
                            color: Theme.panelRaised
                            border.color: versionLabel.activeFocus ? inspector.accent : inspector.panelBorder
                        }
                        onAccepted: {
                            const cleanLabel = text.trim()
                            if (cleanLabel.length > 0 && saveButton.enabled) {
                                inspector.editor.saveVersion(cleanLabel)
                                clear()
                            }
                        }
                    }

                    ShadowButton {
                        id: saveButton
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.controlHeight
                        text: inspector.editor.stateBusy
                            ? qsTr("CREATING…") : qsTr("CREATE VERSION")
                        variant: ShadowButton.Primary
                        enabled: inspector.editor.active && !inspector.editor.stateBusy
                            && versionLabel.text.trim().length > 0
                        onClicked: {
                            inspector.editor.saveVersion(versionLabel.text.trim())
                            versionLabel.clear()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: inspector.panelBorder
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("NAMED VERSIONS")
                            color: inspector.textMuted
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.2
                        }
                        Label {
                            text: qsTr("%L1").arg(versionList.count)
                            color: inspector.textMuted
                            font.pixelSize: 9
                        }
                    }

                    ListView {
                        id: versionList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: inspector.editor.versions
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
                                ? Theme.currentRevisionBorder : inspector.panelBorder

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
                                    color: inspector.textPrimary
                                    font.pixelSize: 11
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    width: parent.width
                                    text: versionRow.changeSummary
                                    color: inspector.textSecondary
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                                Label {
                                    width: parent.width
                                    text: qsTr("%1  ·  %2")
                                        .arg(versionRow.createdAtText)
                                        .arg(versionRow.parentSummary)
                                    color: inspector.textMuted
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
                                    ? (inspector.editor.versionDraft
                                        ? qsTr("LOADED") : qsTr("CURRENT"))
                                    : qsTr("LOAD")
                                color: versionRow.selected ? inspector.accent : inspector.textMuted
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.7
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: !versionRow.selected && !inspector.editor.stateBusy
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: inspector.editor.loadVersionDraft(versionRow.commitId)
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 20
                            visible: versionList.count === 0
                            text: qsTr("Named checkpoints appear here. The current working adjustments are saved automatically; loading a checkpoint never deletes newer work.")
                            color: inspector.textMuted
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
