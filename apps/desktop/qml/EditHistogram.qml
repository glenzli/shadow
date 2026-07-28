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
    readonly property bool stale: scopeData.stale
    readonly property string analysisGeneration:
        scopeData.analysisGeneration
    readonly property bool generationMatches: scopeData.generationMatches
    readonly property bool presentationStale: scopeData.presentationStale
    readonly property bool approximate: scopeData.approximate
    readonly property int sampleWidth: scopeData.sampleWidth
    readonly property int sampleHeight: scopeData.sampleHeight
    readonly property real shadowFraction: scopeData.shadowFraction
    readonly property real highlightFraction: scopeData.highlightFraction
    readonly property real shadowPixels: scopeData.shadowPixels
    readonly property real highlightPixels: scopeData.highlightPixels
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

    function scopeButtonTooltip(mode) {
        switch (mode) {
        case waveformScope:
            return qsTr("Display-referred luminance waveform");
        case paradeScope:
            return qsTr("Display-referred red, green, and blue parade");
        case vectorscopeScope:
            return qsTr("Display-referred chroma distribution with a diagnostic skin-tone guide");
        default:
            return qsTr("Scene-linear RGB histogram and pre-clamp clipping");
        }
    }

    function clippedPercent(fraction) {
        const percent = Math.max(0, Number(fraction)) * 100;
        if (!Number.isFinite(percent) || percent === 0)
            return qsTr("0%");
        if (percent < 0.01)
            return qsTr("<0.01%");
        return qsTr("%1%").arg(
            percent.toLocaleString(Qt.locale(), "f", percent < 1 ? 2 : 1));
    }

    function pixelCountText(value) {
        const count = Math.max(0, Math.round(Number(value)));
        if (!Number.isFinite(count))
            return "0";
        return count.toLocaleString(Qt.locale(), "f", 0);
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

    function clipTooltip(name, pixelCount) {
        const counts = scopeData.binList(name);
        const formattedPixels = root.pixelCountText(pixelCount);
        if (name === "belowZero") {
            return counts.length === 3
                ? qsTr("Any RGB channel below 0 before display clamp · %1 proxy pixels\nR %2 · G %3 · B %4")
                    .arg(formattedPixels)
                    .arg(root.pixelCountText(counts[0]))
                    .arg(root.pixelCountText(counts[1]))
                    .arg(root.pixelCountText(counts[2]))
                : qsTr("Any RGB channel below 0 before display clamp · %1 proxy pixels")
                    .arg(formattedPixels);
        }
        return counts.length === 3
            ? qsTr("Any RGB channel above 1 before display clamp · %1 proxy pixels\nR %2 · G %3 · B %4")
                .arg(formattedPixels)
                .arg(root.pixelCountText(counts[0]))
                .arg(root.pixelCountText(counts[1]))
                .arg(root.pixelCountText(counts[2]))
            : qsTr("Any RGB channel above 1 before display clamp · %1 proxy pixels")
                .arg(formattedPixels);
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

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 28

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 9
                    anchors.rightMargin: 9
                    spacing: 6

                    Label {
                        text: qsTr("ANALYSIS")
                        color: root.mutedTextColor
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.1
                    }

                    Row {
                        spacing: 2

                        Repeater {
                            model: [
                                { mode: root.histogramScope, label: qsTr("H"), name: qsTr("Histogram") },
                                { mode: root.waveformScope, label: qsTr("W"), name: qsTr("Waveform") },
                                { mode: root.paradeScope, label: qsTr("RGB"), name: qsTr("RGB Parade") },
                                { mode: root.vectorscopeScope, label: qsTr("V"), name: qsTr("Vectorscope") }
                            ]

                            delegate: Rectangle {
                                required property var modelData

                                readonly property bool selected: root.scopeMode === modelData.mode
                                width: modelData.mode === root.paradeScope ? 23 : 17
                                height: 16
                                radius: 2
                                color: selected ? Qt.rgba(root.accentColor.r, root.accentColor.g,
                                                          root.accentColor.b, 0.2) : "transparent"
                                border.width: selected ? 1 : 0
                                border.color: selected ? root.accentColor : "transparent"

                                Label {
                                    anchors.centerIn: parent
                                    text: parent.modelData.label
                                    color: parent.selected ? root.accentColor : root.mutedTextColor
                                    font.pixelSize: 7
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0.25
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.scopeMode = parent.modelData.mode
                                    ToolTip.visible: containsMouse
                                    ToolTip.delay: 450
                                    ToolTip.text: root.scopeButtonTooltip(parent.modelData.mode)
                                }
                            }
                        }
                    }

                    Rectangle {
                        visible: root.showingHistogram && root.hasData
                        Layout.preferredWidth: 61
                        Layout.preferredHeight: 16
                        radius: 2
                        color: root.hdrHeadroomPixels > 0
                            ? Qt.rgba(0.93, 0.64, 0.25, 0.16) : "transparent"
                        border.width: root.hdrHeadroomPixels > 0 ? 1 : 0
                        border.color: "#e5a34d"

                        Label {
                            anchors.centerIn: parent
                            text: root.hdrHeadroomText()
                            color: root.hdrHeadroomPixels > 0 ? "#e5a34d" : root.mutedTextColor
                            font.pixelSize: 7
                            font.weight: Font.Bold
                            font.letterSpacing: 0.2
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                            ToolTip.visible: containsMouse
                            ToolTip.delay: 450
                            ToolTip.text: root.hdrHeadroomTooltip()
                        }
                    }

                    Rectangle {
                        visible: root.showingVectorscope
                        Layout.preferredWidth: 35
                        Layout.preferredHeight: 16
                        radius: 2
                        color: root.skinGuideVisible
                            ? Qt.rgba(0.91, 0.49, 0.35, 0.18) : "transparent"
                        border.width: root.skinGuideVisible ? 1 : 0
                        border.color: "#e68b68"

                        Label {
                            anchors.centerIn: parent
                            text: qsTr("SKIN")
                            color: parent.border.width > 0 ? "#e68b68" : root.mutedTextColor
                            font.pixelSize: 7
                            font.weight: Font.Bold
                            font.letterSpacing: 0.35
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.skinGuideVisible = !root.skinGuideVisible
                            ToolTip.visible: containsMouse
                            ToolTip.delay: 450
                            ToolTip.text: root.skinGuideVisible
                                ? qsTr("Hide skin-tone guide") : qsTr("Show skin-tone guide")
                        }
                    }

                    Row {
                        visible: root.showingVectorscope
                        spacing: 2

                        Rectangle {
                            readonly property bool selected: !root.pointColorScopeActive
                            width: 29
                            height: 16
                            radius: 2
                            color: selected
                                ? Qt.rgba(root.accentColor.r, root.accentColor.g,
                                          root.accentColor.b, 0.2) : "transparent"
                            border.width: selected ? 1 : 0
                            border.color: selected ? root.accentColor : "transparent"

                            Label {
                                anchors.centerIn: parent
                                text: qsTr("IMG")
                                color: parent.selected ? root.accentColor : root.mutedTextColor
                                font.pixelSize: 7
                                font.weight: Font.Bold
                                font.letterSpacing: 0.25
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.editor.pointColorScopeActive = false
                                ToolTip.visible: containsMouse
                                ToolTip.delay: 450
                                ToolTip.text: qsTr("Show the complete image in the vectorscope")
                            }
                        }

                        Rectangle {
                            readonly property bool available: !root.beforeView
                                                              && root.editor.pointColorScopeAvailable
                            readonly property bool selected: root.pointColorScopeActive
                            width: 35
                            height: 16
                            radius: 2
                            opacity: available ? 1.0 : 0.45
                            color: selected
                                ? Qt.rgba(root.accentColor.r, root.accentColor.g,
                                          root.accentColor.b, 0.2) : "transparent"
                            border.width: selected ? 1 : 0
                            border.color: selected ? root.accentColor : "transparent"

                            Label {
                                anchors.centerIn: parent
                                text: qsTr("POINT")
                                color: parent.selected ? root.accentColor : root.mutedTextColor
                                font.pixelSize: 7
                                font.weight: Font.Bold
                                font.letterSpacing: 0.2
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: parent.available
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: root.editor.pointColorScopeActive = true
                                ToolTip.visible: containsMouse
                                ToolTip.delay: 450
                                ToolTip.text: parent.available
                                    ? qsTr("Filter the vectorscope by the selected Point Color range")
                                    : qsTr("Select an enabled Point Color to filter the vectorscope")
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    Rectangle {
                        visible: root.updating || root.presentationStale
                        Layout.preferredWidth: 5
                        Layout.preferredHeight: 5
                        radius: 3
                        color: root.updating ? root.accentColor : Theme.textPlaceholder
                    }

                    Label {
                        visible: root.updating || root.presentationStale
                        text: root.updating ? qsTr("UPDATING") : qsTr("STALE")
                        color: root.updating ? root.accentColor : root.mutedTextColor
                        font.pixelSize: 7
                        font.weight: Font.Bold
                        font.letterSpacing: 0.6
                    }

                    Label {
                        text: root.beforeView
                            ? qsTr("BEFORE · WARM PROXY") : qsTr("CURRENT · WARM PROXY")
                        color: root.beforeView ? root.accentColor : root.secondaryTextColor
                        font.pixelSize: 7
                        font.weight: Font.Bold
                        font.letterSpacing: 0.55
                    }
                }
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

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 29
                Layout.leftMargin: 7
                Layout.rightMargin: 7
                spacing: 5
                // These badges describe the same last complete analysis as
                // the plot. Do not pulse or dim them for a transient next
                // generation; that produces a distracting brightness jump
                // across the top of the adjustment sidebar.
                opacity: 1.0

                Rectangle {
                    id: shadowClipBadge

                    Layout.fillWidth: true
                    Layout.preferredHeight: 21
                    radius: 3
                    color: root.hasData && root.shadowPixels > 0
                        ? Theme.shadowClipSurface : Theme.clippingIdleSurface
                    border.color: root.hasData && root.shadowPixels > 0
                                  ? Theme.shadowClipBorder : root.borderColor

                    Label {
                        anchors.centerIn: parent
                        text: root.hasData
                            ? qsTr("◀  SHADOWS  %1").arg(
                                root.clippedPercent(root.shadowFraction))
                            : qsTr("SHADOWS  —")
                        color: root.hasData && root.shadowPixels > 0
                            ? Theme.shadowClipText : root.mutedTextColor
                        font.pixelSize: 8
                        font.weight: Font.Bold
                        font.letterSpacing: 0.35
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                        ToolTip.visible: containsMouse && root.hasData
                        ToolTip.delay: 450
                        ToolTip.text: root.clipTooltip("belowZero", root.shadowPixels)
                    }
                }

                Rectangle {
                    id: highlightClipBadge

                    Layout.fillWidth: true
                    Layout.preferredHeight: 21
                    radius: 3
                    color: root.hasData && root.highlightPixels > 0
                        ? Theme.highlightClipSurface : Theme.clippingIdleSurface
                    border.color: root.hasData && root.highlightPixels > 0
                                  ? Theme.highlightClipBorder : root.borderColor

                    Label {
                        anchors.centerIn: parent
                        text: root.hasData
                            ? qsTr("HIGHLIGHTS  %1  ▶").arg(
                                root.clippedPercent(root.highlightFraction))
                            : qsTr("HIGHLIGHTS  —")
                        color: root.hasData && root.highlightPixels > 0
                            ? Theme.highlightClipText : root.mutedTextColor
                        font.pixelSize: 8
                        font.weight: Font.Bold
                        font.letterSpacing: 0.35
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                        ToolTip.visible: containsMouse && root.hasData
                        ToolTip.delay: 450
                        ToolTip.text: root.clipTooltip("aboveOne", root.highlightPixels)
                    }
                }
            }
        }
    }
}
