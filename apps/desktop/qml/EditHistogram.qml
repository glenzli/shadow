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

    readonly property bool hasData: root.mapBoolean("valid", false) && root.binList("red").length
                                    === 256 && root.binList("green").length === 256 && root.binList(
                                        "blue").length === 256 && root.binList("luma").length
                                    === 256
    readonly property int displayScopeGridSize: root.mapNumber("displayScopeGridSize", 0)
    readonly property int displayScopeBinCount: displayScopeGridSize * displayScopeGridSize
    readonly property bool hasDisplayScope: root.mapBoolean("displayScopeAvailable", false)
                                                && displayScopeGridSize > 0
                                                && root.binList("displayWaveform").length
                                                   === displayScopeBinCount
                                                && root.binList("displayParadeRed").length
                                                   === displayScopeBinCount
                                                && root.binList("displayParadeGreen").length
                                                   === displayScopeBinCount
                                                && root.binList("displayParadeBlue").length
                                                   === displayScopeBinCount
                                                && root.binList("displayVectorscope").length
                                                   === displayScopeBinCount
    readonly property bool showingHistogram: scopeMode === histogramScope
    readonly property bool showingVectorscope: scopeMode === vectorscopeScope
    readonly property bool hasActiveScopeData: showingHistogram ? hasData : hasDisplayScope
    readonly property bool updating: root.mapBoolean("updating", false)
    readonly property bool stale: root.mapBoolean("stale", false)
    readonly property string analysisGeneration: root.generationText(root.field("generation", ""))
    readonly property bool generationMatches: root.hasData && root.displayGeneration.length > 0
                                              && root.analysisGeneration.length > 0
                                              && root.displayGeneration === root.analysisGeneration
    readonly property bool presentationStale: root.stale || (root.hasData &&
                                                             !root.generationMatches)
    readonly property bool approximate: root.mapBoolean("approximate", true)
    readonly property int sampleWidth: root.mapNumber("width", 0)
    readonly property int sampleHeight: root.mapNumber("height", 0)
    readonly property real shadowFraction: root.mapNumber("shadowClippedFraction", 0)
    readonly property real highlightFraction: root.mapNumber("highlightClippedFraction", 0)
    readonly property real shadowPixels: root.mapNumber("shadowClippedPixels", 0)
    readonly property real highlightPixels: root.mapNumber("highlightClippedPixels", 0)
    readonly property real hdrHeadroomPixels: root.mapNumber("hdrHeadroomPixels", 0)
    readonly property real hdrPeakHeadroomEv: root.mapNumber("hdrPeakHeadroomEv", 0)
    readonly property int displayScopeWidth: root.mapNumber("displayScopeWidth", 0)
    readonly property int displayScopeHeight: root.mapNumber("displayScopeHeight", 0)
    readonly property real displayScopeSampledPixels: root.mapNumber("displayScopeSampledPixels", 0)
    readonly property real displayScopeMatchedPixels: root.mapNumber("displayScopeMatchedPixels", 0)
    readonly property bool pointColorScopeActive: root.mapBoolean(
        "displayScopePointColorQualified", false)
    readonly property bool referenceSelection: root.mapBoolean(
        "displayScopeReferenceSelection", false)
    readonly property bool displayScopeCentroidAvailable: root.mapBoolean(
        "displayScopeCentroidAvailable", false)
    readonly property real displayScopeCentroidCb: root.mapNumber("displayScopeCentroidCb", 0)
    readonly property real displayScopeCentroidCr: root.mapNumber("displayScopeCentroidCr", 0)
    readonly property real displayScopeSkinGuideDeviation: root.mapNumber(
        "displayScopeSkinGuideDeviationDegrees", 0)

    // The compact histogram benefits from a short footprint. A vectorscope
    // needs a readable circle, though, so selecting it deliberately gives the
    // diagnostic enough vertical space instead of leaving it as a tiny badge.
    implicitHeight: showingVectorscope ? 318 : 166

    function field(name, fallback) {
        if (root.analysis === null || root.analysis === undefined)
            return fallback;
        const value = root.analysis[name];
        return value === null || value === undefined ? fallback : value;
    }

    function mapBoolean(name, fallback) {
        return Boolean(root.field(name, fallback));
    }

    function mapNumber(name, fallback) {
        const value = Number(root.field(name, fallback));
        return Number.isFinite(value) ? value : fallback;
    }

    function generationText(value) {
        if (value === null || value === undefined)
            return "";
        return String(value);
    }

    function binList(name) {
        const value = root.field(name, []);
        return value !== null && value !== undefined && value.length !== undefined ? value : [];
    }

    function binCount(bins, index) {
        if (index < 0 || index >= bins.length)
            return 0;
        const value = Number(bins[index]);
        return Number.isFinite(value) && value > 0 ? value : 0;
    }

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
        const counts = root.binList(name);
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

    function maximumBinCount() {
        const channels = [root.binList("red"), root.binList("green"), root.binList("blue"), root.binList(
                              "luma")];
        let maximum = 0;
        for (let channel = 0; channel < channels.length; ++channel) {
            for (let index = 0; index < 256; ++index)
                maximum = Math.max(maximum, root.binCount(channels[channel], index));
        }
        return maximum;
    }

    function plotHeight(count, maximum, height) {
        if (count <= 0 || maximum <= 0)
            return 0;
        return Math.log1p(count) / Math.log1p(maximum) * height;
    }

    function fillChannel(context, bins, maximum, chartWidth, chartHeight, color) {
        context.beginPath();
        context.moveTo(0, chartHeight);
        for (let index = 0; index < 256; ++index) {
            const x = index / 255 * chartWidth;
            const y = chartHeight - root.plotHeight(root.binCount(bins, index), maximum,
                                                    chartHeight);

            context.lineTo(x, y);
        }
        context.lineTo(chartWidth, chartHeight);
        context.closePath();
        context.fillStyle = color;
        context.fill();
    }

    function strokeChannel(context, bins, maximum, chartWidth, chartHeight, color) {
        context.beginPath();
        for (let index = 0; index < 256; ++index) {
            const x = index / 255 * chartWidth;
            const y = chartHeight - root.plotHeight(root.binCount(bins, index), maximum,
                                                    chartHeight);

            if (index === 0)
                context.moveTo(x, y);
            else
                context.lineTo(x, y);
        }
        context.lineWidth = 1.15;
        context.lineJoin = "round";
        context.strokeStyle = color;
        context.stroke();
    }

    function maximumScopeCount(lists) {
        let maximum = 0;
        for (let listIndex = 0; listIndex < lists.length; ++listIndex) {
            const bins = lists[listIndex];
            for (let index = 0; index < bins.length; ++index)
                maximum = Math.max(maximum, root.binCount(bins, index));
        }
        return maximum;
    }

    function drawScopeGrid(context, bins, gridSize, x, y, chartWidth, chartHeight, color,
                           maximum) {
        if (maximum <= 0 || gridSize <= 0)
            return;
        const cellWidth = chartWidth / gridSize;
        const cellHeight = chartHeight / gridSize;
        for (let row = 0; row < gridSize; ++row) {
            for (let column = 0; column < gridSize; ++column) {
                const count = root.binCount(bins, row * gridSize + column);
                if (count <= 0)
                    continue;
                const density = Math.log1p(count) / Math.log1p(maximum);
                context.globalAlpha = 0.08 + density * 0.9;
                context.fillStyle = color;
                context.fillRect(x + column * cellWidth, y + row * cellHeight,
                                 Math.max(1, cellWidth + 0.25), Math.max(1, cellHeight + 0.25));
            }
        }
        context.globalAlpha = 1.0;
    }

    function drawScopeGridLines(context, x, y, chartWidth, chartHeight, divisions) {
        context.lineWidth = 1;
        context.strokeStyle = root.histogramGridColor;
        for (let index = 1; index < divisions; ++index) {
            const ratio = index / divisions;
            context.beginPath();
            context.moveTo(x + ratio * chartWidth, y);
            context.lineTo(x + ratio * chartWidth, y + chartHeight);
            context.moveTo(x, y + ratio * chartHeight);
            context.lineTo(x + chartWidth, y + ratio * chartHeight);
            context.stroke();
        }
    }

    function vectorscopeTargetPosition(red, green, blue, centerX, centerY, radius) {
        const luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue
        const cb = (blue - luma) / 1.8556
        const cr = (red - luma) / 1.5748
        return {
            x: centerX + cb * radius * 2,
            y: centerY - cr * radius * 2
        }
    }

    function colorHexComponent(value) {
        const component = Math.round(Math.max(0, Math.min(1, value)) * 255)
            .toString(16)
        return component.length === 1 ? "0" + component : component
    }

    function vectorscopeTraceColor(row, column, gridSize) {
        const cb = column / Math.max(1, gridSize - 1) - 0.5
        const cr = 0.5 - row / Math.max(1, gridSize - 1)
        let red = 0.58 + 1.5748 * cr
        let green = 0.58 - 0.1873 * cb - 0.4681 * cr
        let blue = 0.58 + 1.8556 * cb
        const maximum = Math.max(red, green, blue, 0.0001)
        red = Math.max(0, Math.min(1, red / maximum))
        green = Math.max(0, Math.min(1, green / maximum))
        blue = Math.max(0, Math.min(1, blue / maximum))
        return "#" + colorHexComponent(red) + colorHexComponent(green)
            + colorHexComponent(blue)
    }

    function drawVectorscopeTrace(context, bins, gridSize, centerX, centerY, radius,
                                  maximum) {
        if (maximum <= 0 || gridSize <= 0)
            return
        const cell = radius * 2 / gridSize
        const radiusSquared = radius * radius
        for (let row = 0; row < gridSize; ++row) {
            for (let column = 0; column < gridSize; ++column) {
                const count = root.binCount(bins, row * gridSize + column)
                if (count <= 0)
                    continue
                const x = centerX - radius + column * cell
                const y = centerY - radius + row * cell
                const dx = x + cell * 0.5 - centerX
                const dy = y + cell * 0.5 - centerY
                if (dx * dx + dy * dy > radiusSquared)
                    continue
                const density = Math.log1p(count) / Math.log1p(maximum)
                context.globalAlpha = 0.1 + density * 0.88
                context.fillStyle = root.vectorscopeTraceColor(row, column, gridSize)
                context.fillRect(x, y, Math.max(1, cell + 0.25), Math.max(1, cell + 0.25))
            }
        }
        context.globalAlpha = 1.0
    }

    function drawVectorscopeGraticule(context, centerX, centerY, radius) {
        const targets = [
            { label: "R", red: 1, green: 0, blue: 0, color: "#ef6461" },
            { label: "M", red: 1, green: 0, blue: 1, color: "#d76db8" },
            { label: "B", red: 0, green: 0, blue: 1, color: "#658ee2" },
            { label: "C", red: 0, green: 1, blue: 1, color: "#45c7c7" },
            { label: "G", red: 0, green: 1, blue: 0, color: "#67bb75" },
            { label: "Y", red: 1, green: 1, blue: 0, color: "#e6c35b" }
        ]

        context.lineWidth = 1
        context.strokeStyle = root.histogramGridColor
        for (let ring = 1; ring <= 4; ++ring) {
            context.beginPath()
            context.arc(centerX, centerY, radius * ring / 4, 0, Math.PI * 2)
            context.stroke()
        }

        context.font = "600 9px sans-serif"
        context.textAlign = "center"
        context.textBaseline = "middle"
        for (let index = 0; index < targets.length; ++index) {
            const target = targets[index]
            const position = root.vectorscopeTargetPosition(
                target.red, target.green, target.blue, centerX, centerY, radius)
            const dx = position.x - centerX
            const dy = position.y - centerY
            const length = Math.max(1, Math.sqrt(dx * dx + dy * dy))
            const markerX = centerX + dx / length * radius * 0.86
            const markerY = centerY + dy / length * radius * 0.86
            const labelX = centerX + dx / length * radius * 1.02
            const labelY = centerY + dy / length * radius * 1.02

            context.globalAlpha = 0.45
            context.strokeStyle = target.color
            context.beginPath()
            context.moveTo(centerX, centerY)
            context.lineTo(markerX, markerY)
            context.stroke()
            context.globalAlpha = 1.0
            context.fillStyle = target.color
            context.beginPath()
            context.arc(markerX, markerY, 2.5, 0, Math.PI * 2)
            context.fill()
            context.fillText(target.label, labelX, labelY)
        }
    }

    function skinGuideDeviationText(value) {
        const rounded = Math.round(value)
        return "Δ " + (rounded > 0 ? "+" : "") + rounded + "°"
    }

    function skinToneRange(name) {
        const prefix = "displayScopeSkin" + name
        return {
            available: root.mapBoolean(prefix + "Available", false),
            matchedPixels: root.mapNumber(prefix + "MatchedPixels", 0),
            cb: root.mapNumber(prefix + "CentroidCb", 0),
            cr: root.mapNumber(prefix + "CentroidCr", 0),
            deviation: root.mapNumber(prefix + "DeviationDegrees", 0)
        }
    }

    function drawSkinGuideCorridor(context, centerX, centerY, radius) {
        const guideAngle = Math.atan2(-0.62, -0.48)
        const offsets = [-3, 0, 3]
        for (let index = 0; index < offsets.length; ++index) {
            const angle = guideAngle + offsets[index] * Math.PI / 180
            const boundary = offsets[index] !== 0
            context.globalAlpha = boundary ? 0.34 : 0.9
            context.lineWidth = boundary ? 1 : 1.5
            context.setLineDash(boundary ? [2, 3] : [4, 3])
            context.strokeStyle = "#e68b68"
            context.beginPath()
            context.moveTo(centerX, centerY)
            context.lineTo(
                centerX + Math.cos(angle) * radius * 0.79,
                centerY + Math.sin(angle) * radius * 0.79
            )
            context.stroke()
        }
        context.setLineDash([])
        context.globalAlpha = 1
    }

    function drawSkinToneRangeMarker(context, centerX, centerY, radius,
                                     range, label, color) {
        if (!range.available)
            return
        let x = centerX + range.cb * radius * 2
        let y = centerY - range.cr * radius * 2
        const dx = x - centerX
        const dy = y - centerY
        const distance = Math.sqrt(dx * dx + dy * dy)
        if (distance > radius * 0.94) {
            x = centerX + dx / distance * radius * 0.94
            y = centerY + dy / distance * radius * 0.94
        }
        context.globalAlpha = 0.95
        context.fillStyle = color
        context.beginPath()
        context.arc(x, y, 3, 0, Math.PI * 2)
        context.fill()
        context.strokeStyle = root.plotColor
        context.lineWidth = 1
        context.stroke()
        context.font = "700 7px sans-serif"
        context.textAlign = "left"
        context.fillStyle = color
        context.fillText(label, x + 5, y - 4)
        context.globalAlpha = 1
    }

    function drawSkinToneRanges(context, centerX, centerY, radius) {
        if (!root.pointColorScopeActive)
            return
        root.drawSkinToneRangeMarker(
            context, centerX, centerY, radius,
            root.skinToneRange("Shadows"), "S", "#83a8e5")
        root.drawSkinToneRangeMarker(
            context, centerX, centerY, radius,
            root.skinToneRange("Midtones"), "M", "#ffd1ad")
        root.drawSkinToneRangeMarker(
            context, centerX, centerY, radius,
            root.skinToneRange("Highlights"), "H", "#f2d56f")
    }

    function drawSkinGuideCentroid(context, centerX, centerY, radius) {
        if (!root.pointColorScopeActive || !root.displayScopeCentroidAvailable)
            return
        let x = centerX + root.displayScopeCentroidCb * radius * 2
        let y = centerY - root.displayScopeCentroidCr * radius * 2
        const dx = x - centerX
        const dy = y - centerY
        const distance = Math.sqrt(dx * dx + dy * dy)
        if (distance > radius * 0.94) {
            x = centerX + dx / distance * radius * 0.94
            y = centerY + dy / distance * radius * 0.94
        }
        const labelX = x >= centerX ? x - 38 : x + 8
        const labelY = y < centerY ? y + 11 : y - 7

        context.globalAlpha = 0.82
        context.strokeStyle = "#ffd1ad"
        context.lineWidth = 1
        context.setLineDash([2, 2])
        context.beginPath()
        context.moveTo(centerX, centerY)
        context.lineTo(x, y)
        context.stroke()
        context.setLineDash([])
        context.fillStyle = "#ffd1ad"
        context.beginPath()
        context.arc(x, y, 3.5, 0, Math.PI * 2)
        context.fill()
        context.strokeStyle = "#a65034"
        context.lineWidth = 1.2
        context.beginPath()
        context.arc(x, y, 5.5, 0, Math.PI * 2)
        context.stroke()
        context.font = "600 8px sans-serif"
        context.fillStyle = "#ffd1ad"
        context.fillText(root.skinGuideDeviationText(root.displayScopeSkinGuideDeviation),
                         labelX, labelY)
        context.globalAlpha = 1.0
    }

    onAnalysisChanged: analysisCanvas.requestPaint()
    onVisibleChanged: analysisCanvas.requestPaint()
    onScopeModeChanged: analysisCanvas.requestPaint()
    onHistogramGridColorChanged: analysisCanvas.requestPaint()
    onHistogramRedFillColorChanged: analysisCanvas.requestPaint()
    onHistogramGreenFillColorChanged: analysisCanvas.requestPaint()
    onHistogramBlueFillColorChanged: analysisCanvas.requestPaint()
    onHistogramLumaStrokeColorChanged: analysisCanvas.requestPaint()
    onSkinGuideVisibleChanged: analysisCanvas.requestPaint()

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

                Canvas {
                    id: analysisCanvas

                    anchors.fill: parent
                    anchors.margins: 4
                    // Keep the last complete analysis visually stable while a
                    // replacement proxy is rendering. A full-panel opacity
                    // transition made every slider update look like the
                    // inspector itself was flashing; the compact status badge
                    // above already communicates that the values are pending.
                    opacity: 1.0
                    visible: root.hasActiveScopeData

                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const context = getContext("2d");
                        context.reset();
                        context.clearRect(0, 0, width, height);

                        if (root.showingHistogram) {
                            context.lineWidth = 1;
                            context.strokeStyle = root.histogramGridColor;
                            for (let index = 1; index < 4; ++index) {
                                const x = index / 4 * width;
                                context.beginPath();
                                context.moveTo(x, 0);
                                context.lineTo(x, height);
                                context.stroke();
                            }

                            const maximum = root.maximumBinCount();
                            if (maximum <= 0)
                                return;
                            root.fillChannel(context, root.binList("red"), maximum, width,
                                             height, root.histogramRedFillColor);
                            root.fillChannel(context, root.binList("green"), maximum, width,
                                             height, root.histogramGreenFillColor);
                            root.fillChannel(context, root.binList("blue"), maximum, width,
                                             height, root.histogramBlueFillColor);
                            root.strokeChannel(context, root.binList("luma"), maximum, width,
                                               height, root.histogramLumaStrokeColor);
                            return;
                        }

                        const gridSize = root.displayScopeGridSize;
                        if (gridSize <= 0)
                            return;
                        if (root.scopeMode === root.waveformScope) {
                            const waveform = root.binList("displayWaveform");
                            const maximum = root.maximumScopeCount([waveform]);
                            root.drawScopeGridLines(context, 0, 0, width, height, 4);
                            root.drawScopeGrid(context, waveform, gridSize, 0, 0, width, height,
                                               root.histogramLumaStrokeColor, maximum);
                            return;
                        }
                        if (root.scopeMode === root.paradeScope) {
                            const red = root.binList("displayParadeRed");
                            const green = root.binList("displayParadeGreen");
                            const blue = root.binList("displayParadeBlue");
                            const maximum = root.maximumScopeCount([red, green, blue]);
                            const paradeWidth = width / 3;
                            root.drawScopeGridLines(context, 0, 0, paradeWidth, height, 2);
                            root.drawScopeGridLines(context, paradeWidth, 0, paradeWidth, height,
                                                    2);
                            root.drawScopeGridLines(context, paradeWidth * 2, 0, paradeWidth,
                                                    height, 2);
                            root.drawScopeGrid(context, red, gridSize, 0, 0, paradeWidth, height,
                                               root.histogramRedFillColor, maximum);
                            root.drawScopeGrid(context, green, gridSize, paradeWidth, 0,
                                               paradeWidth, height, root.histogramGreenFillColor,
                                               maximum);
                            root.drawScopeGrid(context, blue, gridSize, paradeWidth * 2, 0,
                                               paradeWidth, height, root.histogramBlueFillColor,
                                               maximum);
                            return;
                        }

                        const vectorscope = root.binList("displayVectorscope");
                        const maximum = root.maximumScopeCount([vectorscope]);
                        const diameter = Math.min(width, height);
                        const radius = Math.max(1, diameter / 2 - 8);
                        const centerX = width / 2;
                        const centerY = height / 2;

                        root.drawVectorscopeGraticule(context, centerX, centerY, radius);
                        context.save();
                        context.beginPath();
                        context.arc(centerX, centerY, radius, 0, Math.PI * 2);
                        context.clip();
                        root.drawVectorscopeTrace(context, vectorscope, gridSize, centerX,
                                                   centerY, radius, maximum);
                        context.restore();

                        // The skin corridor is deliberately a guide rather than a target: it makes
                        // a tint deviation easier to see, while preserving the subject's intended
                        // complexion and lighting. S/M/H markers divide the selected skin samples
                        // by their own relative luminance, not by one complexion-dependent cutoff.
                        if (root.skinGuideVisible) {
                            root.drawSkinGuideCorridor(
                                context, centerX, centerY, radius);
                            root.drawSkinGuideCentroid(context, centerX, centerY, radius);
                            root.drawSkinToneRanges(context, centerX, centerY, radius);
                        }
                    }
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
