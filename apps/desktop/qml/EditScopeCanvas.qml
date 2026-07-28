pragma ComponentBehavior: Bound

import QtQuick

// Draws histogram, waveform, RGB parade, vectorscope, and skin-reference
// diagnostics from one validated EditScopeData projection. No surrounding
// controls or status presentation belong here.
Canvas {
    id: scope

    required property var dataModel
    required property int scopeMode
    required property int histogramScope
    required property int waveformScope
    required property int paradeScope
    required property int vectorscopeScope
    required property bool skinGuideVisible
    required property color plotColor
    required property color histogramGridColor
    required property color histogramRedFillColor
    required property color histogramGreenFillColor
    required property color histogramBlueFillColor
    required property color histogramLumaStrokeColor

    opacity: 1.0

    onDataModelChanged: requestPaint()
    onVisibleChanged: requestPaint()
    onScopeModeChanged: requestPaint()
    onHistogramGridColorChanged: requestPaint()
    onHistogramRedFillColorChanged: requestPaint()
    onHistogramGreenFillColorChanged: requestPaint()
    onHistogramBlueFillColorChanged: requestPaint()
    onHistogramLumaStrokeColorChanged: requestPaint()
    onSkinGuideVisibleChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    Connections {
        target: scope.dataModel
        function onAnalysisChanged() {
            scope.requestPaint();
        }
    }

    function maximumBinCount() {
        const channels = [scope.dataModel.binList("red"), scope.dataModel.binList("green"), scope.dataModel.binList("blue"), scope.dataModel.binList("luma")];
        let maximum = 0;
        for (let channel = 0; channel < channels.length; ++channel) {
            for (let index = 0; index < 256; ++index)
                maximum = Math.max(maximum, scope.dataModel.binCount(channels[channel], index));
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
            const y = chartHeight - scope.plotHeight(scope.dataModel.binCount(bins, index), maximum, chartHeight);

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
            const y = chartHeight - scope.plotHeight(scope.dataModel.binCount(bins, index), maximum, chartHeight);

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
                maximum = Math.max(maximum, scope.dataModel.binCount(bins, index));
        }
        return maximum;
    }

    function drawScopeGrid(context, bins, gridSize, x, y, chartWidth, chartHeight, color, maximum) {
        if (maximum <= 0 || gridSize <= 0)
            return;
        const cellWidth = chartWidth / gridSize;
        const cellHeight = chartHeight / gridSize;
        for (let row = 0; row < gridSize; ++row) {
            for (let column = 0; column < gridSize; ++column) {
                const count = scope.dataModel.binCount(bins, row * gridSize + column);
                if (count <= 0)
                    continue;
                const density = Math.log1p(count) / Math.log1p(maximum);
                context.globalAlpha = 0.08 + density * 0.9;
                context.fillStyle = color;
                context.fillRect(x + column * cellWidth, y + row * cellHeight, Math.max(1, cellWidth + 0.25), Math.max(1, cellHeight + 0.25));
            }
        }
        context.globalAlpha = 1.0;
    }

    function drawScopeGridLines(context, x, y, chartWidth, chartHeight, divisions) {
        context.lineWidth = 1;
        context.strokeStyle = scope.histogramGridColor;
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
        const luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
        const cb = (blue - luma) / 1.8556;
        const cr = (red - luma) / 1.5748;
        return {
            x: centerX + cb * radius * 2,
            y: centerY - cr * radius * 2
        };
    }

    function colorHexComponent(value) {
        const component = Math.round(Math.max(0, Math.min(1, value)) * 255).toString(16);
        return component.length === 1 ? "0" + component : component;
    }

    function vectorscopeTraceColor(row, column, gridSize) {
        const cb = column / Math.max(1, gridSize - 1) - 0.5;
        const cr = 0.5 - row / Math.max(1, gridSize - 1);
        let red = 0.58 + 1.5748 * cr;
        let green = 0.58 - 0.1873 * cb - 0.4681 * cr;
        let blue = 0.58 + 1.8556 * cb;
        const maximum = Math.max(red, green, blue, 0.0001);
        red = Math.max(0, Math.min(1, red / maximum));
        green = Math.max(0, Math.min(1, green / maximum));
        blue = Math.max(0, Math.min(1, blue / maximum));
        return "#" + colorHexComponent(red) + colorHexComponent(green) + colorHexComponent(blue);
    }

    function drawVectorscopeTrace(context, bins, gridSize, centerX, centerY, radius, maximum) {
        if (maximum <= 0 || gridSize <= 0)
            return;
        const cell = radius * 2 / gridSize;
        const radiusSquared = radius * radius;
        for (let row = 0; row < gridSize; ++row) {
            for (let column = 0; column < gridSize; ++column) {
                const count = scope.dataModel.binCount(bins, row * gridSize + column);
                if (count <= 0)
                    continue;
                const x = centerX - radius + column * cell;
                const y = centerY - radius + row * cell;
                const dx = x + cell * 0.5 - centerX;
                const dy = y + cell * 0.5 - centerY;
                if (dx * dx + dy * dy > radiusSquared)
                    continue;
                const density = Math.log1p(count) / Math.log1p(maximum);
                context.globalAlpha = 0.1 + density * 0.88;
                context.fillStyle = scope.vectorscopeTraceColor(row, column, gridSize);
                context.fillRect(x, y, Math.max(1, cell + 0.25), Math.max(1, cell + 0.25));
            }
        }
        context.globalAlpha = 1.0;
    }

    function drawVectorscopeGraticule(context, centerX, centerY, radius) {
        const targets = [
            {
                label: "R",
                red: 1,
                green: 0,
                blue: 0,
                color: "#ef6461"
            },
            {
                label: "M",
                red: 1,
                green: 0,
                blue: 1,
                color: "#d76db8"
            },
            {
                label: "B",
                red: 0,
                green: 0,
                blue: 1,
                color: "#658ee2"
            },
            {
                label: "C",
                red: 0,
                green: 1,
                blue: 1,
                color: "#45c7c7"
            },
            {
                label: "G",
                red: 0,
                green: 1,
                blue: 0,
                color: "#67bb75"
            },
            {
                label: "Y",
                red: 1,
                green: 1,
                blue: 0,
                color: "#e6c35b"
            }
        ];

        context.lineWidth = 1;
        context.strokeStyle = scope.histogramGridColor;
        for (let ring = 1; ring <= 4; ++ring) {
            context.beginPath();
            context.arc(centerX, centerY, radius * ring / 4, 0, Math.PI * 2);
            context.stroke();
        }

        context.font = "600 9px sans-serif";
        context.textAlign = "center";
        context.textBaseline = "middle";
        for (let index = 0; index < targets.length; ++index) {
            const target = targets[index];
            const position = scope.vectorscopeTargetPosition(target.red, target.green, target.blue, centerX, centerY, radius);
            const dx = position.x - centerX;
            const dy = position.y - centerY;
            const length = Math.max(1, Math.sqrt(dx * dx + dy * dy));
            const markerX = centerX + dx / length * radius * 0.86;
            const markerY = centerY + dy / length * radius * 0.86;
            const labelX = centerX + dx / length * radius * 1.02;
            const labelY = centerY + dy / length * radius * 1.02;

            context.globalAlpha = 0.45;
            context.strokeStyle = target.color;
            context.beginPath();
            context.moveTo(centerX, centerY);
            context.lineTo(markerX, markerY);
            context.stroke();
            context.globalAlpha = 1.0;
            context.fillStyle = target.color;
            context.beginPath();
            context.arc(markerX, markerY, 2.5, 0, Math.PI * 2);
            context.fill();
            context.fillText(target.label, labelX, labelY);
        }
    }

    function skinGuideDeviationText(value) {
        const rounded = Math.round(value);
        return "Δ " + (rounded > 0 ? "+" : "") + rounded + "°";
    }

    function drawSkinGuideCorridor(context, centerX, centerY, radius) {
        const guideAngle = Math.atan2(-0.62, -0.48);
        const offsets = [-3, 0, 3];
        for (let index = 0; index < offsets.length; ++index) {
            const angle = guideAngle + offsets[index] * Math.PI / 180;
            const boundary = offsets[index] !== 0;
            context.globalAlpha = boundary ? 0.34 : 0.9;
            context.lineWidth = boundary ? 1 : 1.5;
            context.setLineDash(boundary ? [2, 3] : [4, 3]);
            context.strokeStyle = "#e68b68";
            context.beginPath();
            context.moveTo(centerX, centerY);
            context.lineTo(centerX + Math.cos(angle) * radius * 0.79, centerY + Math.sin(angle) * radius * 0.79);
            context.stroke();
        }
        context.setLineDash([]);
        context.globalAlpha = 1;
    }

    function drawSkinToneRangeMarker(context, centerX, centerY, radius, range, label, color) {
        if (!range.available)
            return;
        let x = centerX + range.cb * radius * 2;
        let y = centerY - range.cr * radius * 2;
        const dx = x - centerX;
        const dy = y - centerY;
        const distance = Math.sqrt(dx * dx + dy * dy);
        if (distance > radius * 0.94) {
            x = centerX + dx / distance * radius * 0.94;
            y = centerY + dy / distance * radius * 0.94;
        }
        context.globalAlpha = 0.95;
        context.fillStyle = color;
        context.beginPath();
        context.arc(x, y, 3, 0, Math.PI * 2);
        context.fill();
        context.strokeStyle = scope.plotColor;
        context.lineWidth = 1;
        context.stroke();
        context.font = "700 7px sans-serif";
        context.textAlign = "left";
        context.fillStyle = color;
        context.fillText(label, x + 5, y - 4);
        context.globalAlpha = 1;
    }

    function drawSkinToneRanges(context, centerX, centerY, radius) {
        if (!scope.dataModel.pointColorScopeActive)
            return;
        scope.drawSkinToneRangeMarker(context, centerX, centerY, radius, scope.dataModel.skinToneRange("Shadows"), "S", "#83a8e5");
        scope.drawSkinToneRangeMarker(context, centerX, centerY, radius, scope.dataModel.skinToneRange("Midtones"), "M", "#ffd1ad");
        scope.drawSkinToneRangeMarker(context, centerX, centerY, radius, scope.dataModel.skinToneRange("Highlights"), "H", "#f2d56f");
    }

    function drawSkinGuideCentroid(context, centerX, centerY, radius) {
        if (!scope.dataModel.pointColorScopeActive || !scope.dataModel.displayScopeCentroidAvailable)
            return;
        let x = centerX + scope.dataModel.displayScopeCentroidCb * radius * 2;
        let y = centerY - scope.dataModel.displayScopeCentroidCr * radius * 2;
        const dx = x - centerX;
        const dy = y - centerY;
        const distance = Math.sqrt(dx * dx + dy * dy);
        if (distance > radius * 0.94) {
            x = centerX + dx / distance * radius * 0.94;
            y = centerY + dy / distance * radius * 0.94;
        }
        const labelX = x >= centerX ? x - 38 : x + 8;
        const labelY = y < centerY ? y + 11 : y - 7;

        context.globalAlpha = 0.82;
        context.strokeStyle = "#ffd1ad";
        context.lineWidth = 1;
        context.setLineDash([2, 2]);
        context.beginPath();
        context.moveTo(centerX, centerY);
        context.lineTo(x, y);
        context.stroke();
        context.setLineDash([]);
        context.fillStyle = "#ffd1ad";
        context.beginPath();
        context.arc(x, y, 3.5, 0, Math.PI * 2);
        context.fill();
        context.strokeStyle = "#a65034";
        context.lineWidth = 1.2;
        context.beginPath();
        context.arc(x, y, 5.5, 0, Math.PI * 2);
        context.stroke();
        context.font = "600 8px sans-serif";
        context.fillStyle = "#ffd1ad";
        context.fillText(scope.skinGuideDeviationText(scope.dataModel.displayScopeSkinGuideDeviation), labelX, labelY);
        context.globalAlpha = 1.0;
    }

    onPaint: {
        const context = getContext("2d");
        context.reset();
        context.clearRect(0, 0, width, height);

        if (scope.scopeMode === scope.histogramScope) {
            context.lineWidth = 1;
            context.strokeStyle = scope.histogramGridColor;
            for (let index = 1; index < 4; ++index) {
                const x = index / 4 * width;
                context.beginPath();
                context.moveTo(x, 0);
                context.lineTo(x, height);
                context.stroke();
            }

            const maximum = scope.maximumBinCount();
            if (maximum <= 0)
                return;
            scope.fillChannel(context, scope.dataModel.binList("red"), maximum, width, height, scope.histogramRedFillColor);
            scope.fillChannel(context, scope.dataModel.binList("green"), maximum, width, height, scope.histogramGreenFillColor);
            scope.fillChannel(context, scope.dataModel.binList("blue"), maximum, width, height, scope.histogramBlueFillColor);
            scope.strokeChannel(context, scope.dataModel.binList("luma"), maximum, width, height, scope.histogramLumaStrokeColor);
            return;
        }

        const gridSize = scope.dataModel.displayScopeGridSize;
        if (gridSize <= 0)
            return;
        if (scope.scopeMode === scope.waveformScope) {
            const waveform = scope.dataModel.binList("displayWaveform");
            const maximum = scope.maximumScopeCount([waveform]);
            scope.drawScopeGridLines(context, 0, 0, width, height, 4);
            scope.drawScopeGrid(context, waveform, gridSize, 0, 0, width, height, scope.histogramLumaStrokeColor, maximum);
            return;
        }
        if (scope.scopeMode === scope.paradeScope) {
            const red = scope.dataModel.binList("displayParadeRed");
            const green = scope.dataModel.binList("displayParadeGreen");
            const blue = scope.dataModel.binList("displayParadeBlue");
            const maximum = scope.maximumScopeCount([red, green, blue]);
            const paradeWidth = width / 3;
            scope.drawScopeGridLines(context, 0, 0, paradeWidth, height, 2);
            scope.drawScopeGridLines(context, paradeWidth, 0, paradeWidth, height, 2);
            scope.drawScopeGridLines(context, paradeWidth * 2, 0, paradeWidth, height, 2);
            scope.drawScopeGrid(context, red, gridSize, 0, 0, paradeWidth, height, scope.histogramRedFillColor, maximum);
            scope.drawScopeGrid(context, green, gridSize, paradeWidth, 0, paradeWidth, height, scope.histogramGreenFillColor, maximum);
            scope.drawScopeGrid(context, blue, gridSize, paradeWidth * 2, 0, paradeWidth, height, scope.histogramBlueFillColor, maximum);
            return;
        }

        const vectorscope = scope.dataModel.binList("displayVectorscope");
        const maximum = scope.maximumScopeCount([vectorscope]);
        const diameter = Math.min(width, height);
        const radius = Math.max(1, diameter / 2 - 8);
        const centerX = width / 2;
        const centerY = height / 2;

        scope.drawVectorscopeGraticule(context, centerX, centerY, radius);
        context.save();
        context.beginPath();
        context.arc(centerX, centerY, radius, 0, Math.PI * 2);
        context.clip();
        scope.drawVectorscopeTrace(context, vectorscope, gridSize, centerX, centerY, radius, maximum);
        context.restore();

        // The skin corridor is deliberately a guide rather than a target: it makes
        // a tint deviation easier to see, while preserving the subject's intended
        // complexion and lighting. S/M/H markers divide the selected skin samples
        // by their own relative luminance, not by one complexion-dependent cutoff.
        if (scope.skinGuideVisible) {
            scope.drawSkinGuideCorridor(context, centerX, centerY, radius);
            scope.drawSkinGuideCentroid(context, centerX, centerY, radius);
            scope.drawSkinToneRanges(context, centerX, centerY, radius);
        }
    }
}
