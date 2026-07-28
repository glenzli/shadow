pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var analysis
    required property var editor
    property bool beforeView: false
    property string displayGeneration: ""

    property color panelColor: Theme.chrome
    property color plotColor: Theme.plot
    property color borderColor: Theme.border
    property color textColor: Theme.textPrimary
    property color secondaryTextColor: Theme.textSecondary
    property color mutedTextColor: Theme.textMuted
    property color accentColor: Theme.accent
    property color histogramGridColor: Theme.histogramGrid
    property color histogramRedFillColor: Theme.histogramRedFill
    property color histogramGreenFillColor: Theme.histogramGreenFill
    property color histogramBlueFillColor: Theme.histogramBlueFill
    property color histogramLumaStrokeColor: Theme.histogramLumaStroke

    readonly property int histogramScope: 0
    readonly property int waveformScope: 1
    readonly property int paradeScope: 2
    readonly property int vectorscopeScope: 3
    property int scopeMode: histogramScope
    property bool skinGuideVisible: true

    EditScopeData {
        id: scopeData
        analysis: root.analysis
        displayGeneration: root.displayGeneration
    }

    readonly property bool hasData: scopeData.hasHistogramData
    readonly property int displayScopeGridSize:
        scopeData.displayScopeGridSize
    readonly property int displayScopeBinCount:
        scopeData.displayScopeBinCount
    readonly property bool hasDisplayScope: scopeData.hasDisplayScope
    readonly property bool showingHistogram: scopeMode === histogramScope
    readonly property bool showingVectorscope: scopeMode === vectorscopeScope
    readonly property bool hasActiveScopeData: showingHistogram ? hasData : hasDisplayScope
    readonly property bool updating: scopeData.updating
    readonly property bool presentationStale: scopeData.presentationStale
    readonly property bool approximate: scopeData.approximate
    readonly property int sampleWidth: scopeData.sampleWidth
    readonly property int sampleHeight: scopeData.sampleHeight
    readonly property real hdrHeadroomPixels: scopeData.hdrHeadroomPixels
    readonly property real hdrPeakHeadroomEv: scopeData.hdrPeakHeadroomEv
    readonly property int displayScopeWidth: scopeData.displayScopeWidth
    readonly property int displayScopeHeight: scopeData.displayScopeHeight
    readonly property real displayScopeSampledPixels:
        scopeData.displayScopeSampledPixels
    readonly property real displayScopeMatchedPixels:
        scopeData.displayScopeMatchedPixels
    readonly property bool pointColorScopeActive:
        scopeData.pointColorScopeActive
    readonly property bool referenceSelection: scopeData.referenceSelection
    readonly property bool displayScopeCentroidAvailable:
        scopeData.displayScopeCentroidAvailable
    readonly property real displayScopeCentroidCb:
        scopeData.displayScopeCentroidCb
    readonly property real displayScopeCentroidCr:
        scopeData.displayScopeCentroidCr
    readonly property real displayScopeSkinGuideDeviation:
        scopeData.displayScopeSkinGuideDeviation

    // The compact histogram benefits from a short footprint. A vectorscope
    // needs a readable circle, though, so selecting it deliberately gives the
    // diagnostic enough vertical space instead of leaving it as a tiny badge.
    implicitHeight: showingVectorscope ? 318 : 166

    function activeScopeLabel() {
        switch (scopeMode) {
        case waveformScope:
            return qsTr("WAVEFORM");
        case paradeScope:
            return qsTr("RGB PARADE");
        case vectorscopeScope:
            return qsTr("VECTORSCOPE");
        default:
            return qsTr("HISTOGRAM");
        }
    }

    function pixelCountText(value) {
        const count = Math.max(0, Math.round(Number(value)));
        if (!Number.isFinite(count))
            return "0";
        return count.toLocaleString(Qt.locale(), "f", 0);
    }

    // Point Color consumes the public analysis-scope contract, while
    // EditScopeData remains this component's private defensive projection.
    function skinToneRange(name) {
        return scopeData.skinToneRange(name);
    }

    function hdrHeadroomText() {
        if (root.hdrHeadroomPixels <= 0 || root.hdrPeakHeadroomEv <= 0)
            return qsTr("HDR —");
        return qsTr("HDR +%1 EV").arg(
            root.hdrPeakHeadroomEv.toLocaleString(Qt.locale(), "f", 1));
    }

    function hdrHeadroomTooltip() {
        return qsTr("Peak linear luminance before SDR display mapping · %1 proxy pixels above display white\nThis is output headroom, not sensor dynamic range.")
            .arg(root.pixelCountText(root.hdrHeadroomPixels));
    }


    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.topMargin: 8
        anchors.bottomMargin: 4
        radius: 0
        color: root.panelColor
        border.width: 0
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            EditScopeToolbar {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                editor: root.editor
                beforeView: root.beforeView
                scopeMode: root.scopeMode
                histogramScope: root.histogramScope
                waveformScope: root.waveformScope
                paradeScope: root.paradeScope
                vectorscopeScope: root.vectorscopeScope
                skinGuideVisible: root.skinGuideVisible
                showingHistogram: root.showingHistogram
                showingVectorscope: root.showingVectorscope
                pointColorScopeActive: root.pointColorScopeActive
                hasData: root.hasData
                updating: root.updating
                presentationStale: root.presentationStale
                hdrHeadroomPixels: root.hdrHeadroomPixels
                hdrHeadroomText: root.hdrHeadroomText()
                hdrHeadroomTooltip: root.hdrHeadroomTooltip()
                accentColor: root.accentColor
                secondaryTextColor: root.secondaryTextColor
                mutedTextColor: root.mutedTextColor
                onScopeModeRequested: mode => root.scopeMode = mode
                onSkinGuideVisibleRequested: visible =>
                    root.skinGuideVisible = visible
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 7
                Layout.rightMargin: 7
                color: root.plotColor
                border.color: Theme.histogramFrameBorder
                clip: true

                EditScopeCanvas {
                    id: analysisCanvas
                    anchors.fill: parent
                    anchors.margins: 4
                    dataModel: scopeData
                    scopeMode: root.scopeMode
                    histogramScope: root.histogramScope
                    waveformScope: root.waveformScope
                    paradeScope: root.paradeScope
                    vectorscopeScope: root.vectorscopeScope
                    skinGuideVisible: root.skinGuideVisible
                    plotColor: root.plotColor
                    histogramGridColor: root.histogramGridColor
                    histogramRedFillColor: root.histogramRedFillColor
                    histogramGreenFillColor: root.histogramGreenFillColor
                    histogramBlueFillColor: root.histogramBlueFillColor
                    histogramLumaStrokeColor:
                        root.histogramLumaStrokeColor
                    visible: root.hasActiveScopeData
                }

                Label {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 5
                    visible: root.hasActiveScopeData
                    text: root.showingHistogram
                        ? qsTr("LOG")
                        : (root.referenceSelection
                            ? qsTr("SKIN REFERENCE · LOCKED")
                            : root.pointColorScopeActive ? qsTr("POINT COLOR · DISPLAY")
                                                         : qsTr("DISPLAY PREVIEW"))
                    color: Theme.textSubtle
                    font.pixelSize: 6
                    font.weight: Font.Bold
                    font.letterSpacing: 0.5
                }

                Label {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 5
                    visible: root.hasActiveScopeData
                    text: root.showingHistogram
                        ? (root.approximate
                            ? qsTr("%L1×%L2 · APPROX")
                                .arg(root.sampleWidth).arg(root.sampleHeight)
                            : qsTr("%L1×%L2").arg(root.sampleWidth).arg(root.sampleHeight))
                        : (root.pointColorScopeActive
                            ? qsTr("%L1×%L2 · %L3 MATCHED")
                                .arg(root.displayScopeWidth)
                                .arg(root.displayScopeHeight)
                                .arg(root.pixelCountText(root.displayScopeMatchedPixels))
                            : qsTr("%L1×%L2 · %L3")
                                .arg(root.displayScopeWidth)
                                .arg(root.displayScopeHeight)
                                .arg(root.pixelCountText(root.displayScopeSampledPixels)))
                    color: Theme.textSubtle
                    font.pixelSize: 6
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.35
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: !root.hasActiveScopeData
                    text: root.updating
                        ? qsTr("Analyzing the warm preview…")
                        : qsTr("%1 unavailable").arg(root.activeScopeLabel())
                    color: root.updating ? root.secondaryTextColor : root.mutedTextColor
                    font.pixelSize: 9
                    horizontalAlignment: Text.AlignHCenter
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: root.showingVectorscope && root.hasActiveScopeData
                             && root.pointColorScopeActive
                             && root.displayScopeMatchedPixels <= 0
                    text: qsTr("No pixels match the selected Point Color")
                    color: root.mutedTextColor
                    font.pixelSize: 9
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            EditScopeClipSummary {
                Layout.fillWidth: true
                Layout.preferredHeight: 29
                Layout.leftMargin: 7
                Layout.rightMargin: 7
                // These badges describe the same last complete analysis as
                // the plot. Do not pulse or dim them for a transient next
                // generation; that produces a distracting brightness jump
                // across the top of the adjustment sidebar.
                dataModel: scopeData
                borderColor: root.borderColor
                mutedTextColor: root.mutedTextColor
            }
        }
    }
}
