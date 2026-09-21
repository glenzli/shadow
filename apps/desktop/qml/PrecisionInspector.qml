pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

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
    required property bool maskOverlayVisible
    required property bool selectedRetouchContinuous
    required property int selectedRetouchIndex
    required property real currentPhotoAspect
    required property real workspaceWidth
    required property color panel
    required property color panelRaised
    required property color panelBorder
    required property color textPrimary
    required property color textSecondary
    required property color textMuted
    required property color accent

    signal openLutLibraryRequested
    signal openOpticsProfileLibraryRequested
    signal toolModeRequested(int mode)
    signal toolFinishRequested()
    signal cropAspectRatioRequested(real ratio)
    signal maskOverlayVisibilityRequested(bool visible)
    signal retouchRegionSelectionRequested(bool continuous, int index)
    signal colorWarperRequested()

    readonly property int toolNone: 0
    readonly property int toolMask: 1
    readonly property int toolCrop: 2
    readonly property int toolRepair: 3
    readonly property int toolLiquify: 4
    readonly property int toolCompletion: 5
    readonly property int toolPaint: 6

    function manualOpticsActive() {
        return Number(editor.manualOpticsDistortion) !== 0 || Number(editor.manualOpticsTcaRedCyan) !== 0 || Number(editor.manualOpticsTcaBlueYellow) !== 0 || Number(editor.manualOpticsVignettingAmount) !== 0;
    }

    function fineValue(key) {
        // Reading the revision makes generic key lookups reactive without
        // exposing dozens of one-off Q_PROPERTY accessors.
        const revision = editor.parameterRevision;
        return revision >= 0 ? editor.parameterValue(key) : 0;
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

        PrecisionCaptureMetadata {
            Layout.fillWidth: true
            Layout.preferredHeight: 58
            captureMetadata: inspector.captureMetadata
            representationId: inspector.editor.representationId
            panelColor: inspector.panel
            borderColor: inspector.panelBorder
            textPrimary: inspector.textPrimary
            textSecondary: inspector.textSecondary
            textMuted: inspector.textMuted
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
                    id: maskToolButton
                    buttonSize: 32
                    iconSize: 19
                    source: selected ? "qrc:/icons/mask.svg" : "qrc:/icons/mask-create.svg"
                    selected: inspector.activeToolMode === inspector.toolMask
                    toolTipText: selected ? qsTr("Finish mask editing") : qsTr("Create or edit a node mask")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active && inspector.editor.hasSelectedGradeNode && !inspector.editor.stateBusy
                    onClicked: {
                        if (selected) {
                            inspector.toolModeRequested(inspector.toolMask);
                        } else {
                            maskCreateMenu.openFor(maskToolButton, maskCreateMenu.newNodeDestination);
                        }
                    }
                }

                ShadowIconButton {
                    buttonSize: 32
                    iconSize: 19
                    source: "qrc:/icons/crop.svg"
                    selected: inspector.activeToolMode === inspector.toolCrop
                    toolTipText: qsTr("Crop and straighten")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active && inspector.previewFrameReady && !inspector.editor.stateBusy
                    onClicked: inspector.toolModeRequested(inspector.toolCrop)
                }

                ShadowIconButton {
                    buttonSize: 32
                    iconSize: 19
                    source: "qrc:/icons/retouch.svg"
                    selected: inspector.activeToolMode === inspector.toolRepair
                    toolTipText: qsTr("Repair")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active && inspector.previewFrameReady && !inspector.editor.stateBusy
                    onClicked: inspector.toolModeRequested(inspector.toolRepair)
                }

                ShadowIconButton {
                    buttonSize: 32
                    iconSize: 19
                    source: "qrc:/icons/brush.svg"
                    selected: inspector.activeToolMode === inspector.toolLiquify
                    toolTipText: qsTr("Liquify")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active && inspector.previewFrameReady
                        && !inspector.editor.stateBusy
                    onClicked: inspector.toolModeRequested(inspector.toolLiquify)
                }

                ShadowIconButton {
                    buttonSize: 32
                    iconSize: 19
                    source: "qrc:/icons/candidate.svg"
                    selected: inspector.activeToolMode === inspector.toolCompletion
                    toolTipText: qsTr("AI Completion")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active && inspector.previewFrameReady
                        && (!inspector.editor.stateBusy || selected)
                    onClicked:
                        inspector.toolModeRequested(inspector.toolCompletion)
                }

                ShadowIconButton {
                    objectName: "paintToolButton"
                    buttonSize: 32
                    iconSize: 19
                    source: "qrc:/icons/brush.svg"
                    selected: inspector.activeToolMode === inspector.toolPaint
                    toolTipText: qsTr("Paint · color repair and light shaping")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active && inspector.previewFrameReady && !inspector.editor.stateBusy
                    onClicked: inspector.toolModeRequested(inspector.toolPaint)
                }

                Item {
                    Layout.fillWidth: true
                }

                Rectangle {
                    visible: inspector.activeToolMode === inspector.toolNone
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 20
                    color: inspector.panelBorder
                }

                ShadowIconButton {
                    visible: inspector.activeToolMode === inspector.toolMask
                    buttonSize: 32
                    iconSize: 19
                    source: inspector.maskOverlayVisible ? "qrc:/icons/overlay-show.svg" : "qrc:/icons/overlay-hide.svg"
                    selected: inspector.maskOverlayVisible
                    toolTipText: inspector.maskOverlayVisible ? qsTr("Hide mask overlay · O") : qsTr("Show mask overlay · O")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active
                    onClicked: inspector.maskOverlayVisibilityRequested(!inspector.maskOverlayVisible)
                }

                ShadowIconButton {
                    visible: inspector.activeToolMode !== inspector.toolNone
                    buttonSize: 32
                    iconSize: 20
                    source: "qrc:/icons/check.svg"
                    variant: ShadowIconButton.Ghost
                    foregroundColor: inspector.accent
                    toolTipText: qsTr("Exit this tool and keep its adjustments")
                    accessibleName: toolTipText
                    onClicked: inspector.toolFinishRequested()
                }

                ShadowIconButton {
                    visible: inspector.activeToolMode === inspector.toolNone
                    buttonSize: 32
                    iconSize: 19
                    source: "qrc:/icons/reset-all.svg"
                    variant: ShadowIconButton.Ghost
                    foregroundColor: Theme.dangerText
                    toolTipText: qsTr("Reset all adjustments…")
                    accessibleName: toolTipText
                    enabled: inspector.editor.active && !inspector.editor.stateBusy
                    onClicked: resetAllDialog.open()
                }
            }
        }

        Rectangle {
            id: inspectorTabStrip
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 38 : 0
            visible: inspector.activeToolMode === inspector.toolNone
                && inspector.editor.selectedRecipeNodeKind !== "completion"
            color: inspector.panel

            property int currentIndex: 0

            function selectTab(index) {
                if (currentIndex === index)
                    return;
                currentIndex = index;
                Qt.callLater(function () {
                    if (inspectorScroll.contentItem)
                        inspectorScroll.contentItem.contentY = 0;
                });
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
                        width: inspectorScroll.availableWidth
                        spacing: 7
                        enabled: inspector.editor.active

                        PrecisionSubjectEmphasis {
                            editor: inspector.editor
                            visible: inspectorTabStrip.currentIndex === 0
                                && inspector.editor.selectedRecipeNodeKind !== "completion"
                            Layout.topMargin: 10
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 7
                            enabled: !inspector.editor.stateBusy

                            PrecisionFoundationAdjustments {
                                Layout.fillWidth: true
                                visible: !inspector.editor.rawDenoiseSelected
                                    && inspector.editor.selectedRecipeNodeKind !== "completion"
                                    && inspectorTabStrip.currentIndex === 0
                                editor: inspector.editor
                                gradeControlsEnabled: inspector.editor.gradeNodeEnabled
                                panelRaised: inspector.panelRaised
                                panelBorder: inspector.panelBorder
                                textPrimary: inspector.textPrimary
                                textMuted: inspector.textMuted
                                accent: inspector.accent
                            }

                            PrecisionRawDenoiseAdjustments {
                                Layout.fillWidth: true
                                visible: inspector.editor.rawDenoiseSelected
                                    && inspectorTabStrip.currentIndex === 0
                                editor: inspector.editor
                                textPrimary: inspector.textPrimary
                                textMuted: inspector.textMuted
                                accent: inspector.accent
                            }

                            PrecisionAiCompletionTools {
                                Layout.fillWidth: true
                                Layout.leftMargin: 12
                                Layout.rightMargin: 12
                                Layout.topMargin: 12
                                visible:
                                    inspector.editor.selectedRecipeNodeKind === "completion"
                                editor: inspector.editor
                                authoring: false
                                onStartRequested:
                                    inspector.toolModeRequested(inspector.toolCompletion)
                            }

                            ColumnLayout {
                                objectName: "gradeNodeInspector"
                                Layout.fillWidth: true
                                visible: !inspector.editor.rawDenoiseSelected
                                    && inspector.editor.selectedRecipeNodeKind !== "completion"
                                spacing: 8
                                enabled: inspector.editor.foundationSelected
                                    || inspector.editor.gradeNodeEnabled
                                opacity: enabled ? 1.0 : 0.42

                                Behavior on opacity {
                                    NumberAnimation {
                                        duration: 100
                                    }
                                }

                                ShadowAdjustmentSection {
                                    Layout.fillWidth: true
                                    visible: !inspector.editor.foundationSelected
                                        && inspectorTabStrip.currentIndex === 0
                                    title: qsTr("NODE STRENGTH")
                                    summary: qsTr("%1%").arg(
                                        Math.round(inspector.editor.gradeNodeStrength * 100))
                                    toolTipText: qsTr("Blend the complete Grade Node with its input. Zero bypasses the node; the adjustment graph is evaluated only once.")
                                    resetAvailable: true
                                    resetEnabled: Math.abs(inspector.editor.gradeNodeStrength - 1.0) > 0.000001
                                    onResetRequested: inspector.editor.gradeNodeStrength = 1.0

                                    ShadowSlider {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 14
                                        Layout.rightMargin: 14
                                        label: qsTr("Strength")
                                        from: 0
                                        to: 1
                                        neutralValue: 1
                                        fillFromMinimum: true
                                        stepSize: 0.01
                                        decimals: 0
                                        displayMultiplier: 100
                                        suffix: "%"
                                        value: inspector.editor.gradeNodeStrength
                                        onGestureStarted: inspector.editor.beginParameterEdit(
                                            "node/strength")
                                        onEdited: value => inspector.editor.gradeNodeStrength = value
                                        onGestureFinished: inspector.editor.endParameterEdit(
                                            "node/strength")
                                    }
                                }

                                PrecisionColorMixer {
                                    Layout.fillWidth: true
                                    visible: !inspector.editor.foundationSelected
                                        && inspectorTabStrip.currentIndex === 0
                                    editor: inspector.editor
                                    panelRaised: inspector.panelRaised
                                    panelBorder: inspector.panelBorder
                                    textPrimary: inspector.textPrimary
                                    textMuted: inspector.textMuted
                                    accent: inspector.accent
                                }

                                PrecisionSelectiveColor {
                                    Layout.fillWidth: true
                                    visible: !inspector.editor.foundationSelected
                                        && inspectorTabStrip.currentIndex === 0
                                    editor: inspector.editor
                                    panelBorder: inspector.panelBorder
                                }

                                PrecisionPointColorSection {
                                    Layout.fillWidth: true
                                    visible: !inspector.editor.foundationSelected
                                        && inspectorTabStrip.currentIndex === 0
                                    editor: inspector.editor
                                    analysisScope: analysisScope
                                    previewFrameReady: inspector.previewFrameReady
                                    readyPreviewGeneration: inspector.readyPreviewGeneration
                                    comparisonActive: inspector.comparisonActive
                                    accent: inspector.accent
                                }

                                ShadowAdjustmentSection {
                                    Layout.fillWidth: true
                                    visible: !inspector.editor.foundationSelected
                                        && inspectorTabStrip.currentIndex === 0
                                    title: qsTr("COLOR MAP")
                                    summary: qsTr("OKLAB 5×5")
                                    toolTipText: qsTr("Move a smooth connected Oklab mesh after Color Mixer and Point Color. This is a separate chroma-field correction, not a hue-keyed slider.")
                                    resetAvailable: true
                                    onResetRequested: inspector.editor.resetColorWarper()
                                    headerActions: Component {
                                        ShadowIconButton {
                                            objectName: "expandColorWarper"
                                            buttonSize: 24
                                            iconSize: 16
                                            source: "qrc:/icons/fit-view.svg"
                                            toolTipText: qsTr("Expand color map")
                                            accessibleName: toolTipText
                                            enabled: inspector.editor.active && inspector.editor.gradeNodeEnabled
                                            onClicked: inspector.colorWarperRequested()
                                        }
                                    }

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
                                    visible: !inspector.editor.foundationSelected
                                        && inspectorTabStrip.currentIndex === 1
                                    editor: inspector.editor
                                    lutLibrary: inspector.lutLibrary
                                    textPrimary: inspector.textPrimary
                                    textSecondary: inspector.textSecondary
                                    textMuted: inspector.textMuted
                                    accent: inspector.accent
                                    onOpenLibraryRequested: inspector.openLutLibraryRequested()
                                }

                                ShadowAdjustmentSection {
                                    Layout.fillWidth: true
                                    visible: !inspector.editor.foundationSelected
                                        && inspectorTabStrip.currentIndex === 1
                                    title: qsTr("COLOR GRADING")
                                    toolTipText: qsTr("Tint shadows, midtones, and highlights independently with perceptual color wheels.")
                                    resetAvailable: true
                                    onResetRequested:
                                        inspector.editor.resetSelectedAdjustmentSection(
                                            "color_grading")

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 12
                                        Layout.rightMargin: 12
                                        spacing: 8

                                        Repeater {
                                            model: [
                                                {
                                                    "range": "shadows",
                                                    "label": qsTr("Shadows"),
                                                    "hue": "shadows_hue",
                                                    "saturation": "shadows_saturation",
                                                    "luminance": "shadows_luminance"
                                                },
                                                {
                                                    "range": "midtones",
                                                    "label": qsTr("Midtones"),
                                                    "hue": "midtones_hue",
                                                    "saturation": "midtones_saturation",
                                                    "luminance": "midtones_luminance"
                                                },
                                                {
                                                    "range": "highlights",
                                                    "label": qsTr("Highlights"),
                                                    "hue": "highlights_hue",
                                                    "saturation": "highlights_saturation",
                                                    "luminance": "highlights_luminance"
                                                }
                                            ]
                                            delegate: ShadowColorWheel {
                                                required property var modelData
                                                Layout.fillWidth: true
                                                label: modelData.label
                                                hue: inspector.fineValue(modelData.hue)
                                                saturation: inspector.fineValue(modelData.saturation)
                                                luminance: inspector.fineValue(modelData.luminance)
                                                onWheelGestureStarted: inspector.editor.beginParameterEdit("color_grading/" + modelData.range + "/wheel")
                                                onWheelEdited: (hue, saturation) => inspector.editor.setColorGradingWheel(modelData.range, hue, saturation)
                                                onWheelGestureFinished: inspector.editor.endParameterEdit("color_grading/" + modelData.range + "/wheel")
                                                onLuminanceGestureStarted: inspector.editor.beginParameterEdit(modelData.luminance)
                                                onLuminanceEdited: value => inspector.editor.setParameterValue(modelData.luminance, value)
                                                onLuminanceGestureFinished: inspector.editor.endParameterEdit(modelData.luminance)
                                            }
                                        }
                                    }

                                    Repeater {
                                        model: [
                                            {
                                                "key": "grading_blending",
                                                "name": qsTr("Blending"),
                                                "from": 0,
                                                "neutral": 0.5
                                            },
                                            {
                                                "key": "grading_balance",
                                                "name": qsTr("Balance"),
                                                "from": -1,
                                                "neutral": 0
                                            }
                                        ]
                                        delegate: ShadowSlider {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 14
                                            Layout.rightMargin: 14
                                            label: modelData.name
                                            from: modelData.from
                                            to: 1
                                            neutralValue: modelData.neutral
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

                                PrecisionTechnicalTools {
                                    Layout.fillWidth: true
                                    inspector: inspector
                                    currentTabIndex: inspectorTabStrip.currentIndex
                                    foundationSelected: inspector.editor.foundationSelected
                                    onOpenOpticsProfileLibraryRequested: inspector.openOpticsProfileLibraryRequested()
                                }
                            }

                            Item {
                                Layout.preferredHeight: 14
                            }
                        }
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
                        width: specialToolScroll.availableWidth
                        spacing: 0
                        enabled: inspector.editor.active
                            && (!inspector.editor.stateBusy
                                || inspector.activeToolMode
                                    === inspector.toolCompletion)

                        PrecisionLocalMaskTools {
                            Layout.fillWidth: true
                            visible: inspector.activeToolMode === inspector.toolMask
                            inspector: inspector
                            currentTabIndex: 0
                            onCreateMaskRequested: anchorItem => maskCreateMenu.openFor(anchorItem, maskCreateMenu.currentNodeDestination)
                        }

                        PrecisionGeometryTools {
                            Layout.fillWidth: true
                            visible: inspector.activeToolMode === inspector.toolCrop
                            inspector: inspector
                            currentTabIndex: 0
                            aspectRatioLock: inspector.cropAspectRatioLock
                            onAspectRatioRequested: ratio => inspector.cropAspectRatioRequested(ratio)
                        }

                        PrecisionRetouchTools {
                            Layout.fillWidth: true
                            visible: inspector.activeToolMode === inspector.toolRepair
                            inspector: inspector
                            currentTabIndex: 0
                            selectedRegionContinuous: inspector.selectedRetouchContinuous
                            selectedRegionIndex: inspector.selectedRetouchIndex
                            onRegionSelectionRequested: (continuous, index) => inspector.retouchRegionSelectionRequested(continuous, index)
                        }

                        PrecisionLiquifyTools {
                            Layout.fillWidth: true
                            visible: inspector.activeToolMode
                                === inspector.toolLiquify
                            inspector: inspector
                            currentTabIndex: 0
                        }

                        PrecisionPaintTools {
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.topMargin: 12
                            visible: inspector.activeToolMode === inspector.toolPaint
                            editor: inspector.editor
                        }

                        PrecisionAiCompletionTools {
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.topMargin: 12
                            visible: inspector.activeToolMode
                                === inspector.toolCompletion
                            editor: inspector.editor
                            authoring: true
                            onExitRequested:
                                inspector.toolFinishRequested()
                        }

                        Item {
                            Layout.preferredHeight: 14
                        }
                    }
                }
            }
        }
    }

    PrecisionMaskCreateMenu {
        id: maskCreateMenu
        editor: inspector.editor
        onMaskCreated: {
            if (inspector.activeToolMode !== inspector.toolMask)
                inspector.toolModeRequested(inspector.toolMask);
        }
        onAiMaskRequested: {
            if (inspector.activeToolMode !== inspector.toolMask)
                inspector.toolModeRequested(inspector.toolMask);
        }
        onEditExistingRequested: {
            if (inspector.activeToolMode !== inspector.toolMask)
                inspector.toolModeRequested(inspector.toolMask);
        }
    }

    PrecisionResetAllDialog {
        id: resetAllDialog
        editor: inspector.editor
        hostWidth: inspector.workspaceWidth
    }
}
