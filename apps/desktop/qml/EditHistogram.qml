pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var analysis
    property bool beforeView: false
    property string displayGeneration: ""

    property color panelColor: "#101317"
    property color plotColor: "#0b0e11"
    property color borderColor: "#2a3037"
    property color textColor: "#edf0f2"
    property color secondaryTextColor: "#bdc4ca"
    property color mutedTextColor: "#8b949e"
    property color accentColor: "#d8b36a"

    readonly property bool hasData: root.mapBoolean("valid", false) && root.binList("red").length
                                    === 256 && root.binList("green").length === 256 && root.binList(
                                        "blue").length === 256 && root.binList("luma").length
                                    === 256
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

    implicitHeight: 166

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

    function clippedPercent(fraction) {
        const percent = Math.max(0, Number(fraction)) * 100;
        if (!Number.isFinite(percent) || percent === 0)
            return "0%";
        if (percent < 0.01)
            return "<0.01%";
        return percent.toFixed(percent < 1 ? 2 : 1) + "%";
    }

    function pixelCountText(value) {
        const count = Math.max(0, Math.round(Number(value)));
        if (!Number.isFinite(count))
            return "0";
        return count.toLocaleString(Qt.locale(), "f", 0);
    }

    function channelClipText(name) {
        const counts = root.binList(name);
        if (counts.length !== 3)
            return "";
        return "\nR " + root.pixelCountText(counts[0])
            + " · G " + root.pixelCountText(counts[1])
            + " · B " + root.pixelCountText(counts[2]);
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

    onAnalysisChanged: histogramCanvas.requestPaint()
    onVisibleChanged: histogramCanvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        radius: 4
        color: root.panelColor
        border.color: root.borderColor
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
                        text: "HISTOGRAM"
                        color: root.mutedTextColor
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.1
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    Rectangle {
                        visible: root.updating || root.presentationStale
                        Layout.preferredWidth: 5
                        Layout.preferredHeight: 5
                        radius: 3
                        color: root.updating ? root.accentColor : "#68727c"

                        SequentialAnimation on opacity {
                            running: root.updating
                            loops: Animation.Infinite
                            NumberAnimation {
                                to: 0.25
                                duration: 480
                            }
                            NumberAnimation {
                                to: 1.0
                                duration: 480
                            }
                        }
                    }

                    Label {
                        visible: root.updating || root.presentationStale
                        text: root.updating ? "UPDATING" : "STALE"
                        color: root.updating ? root.accentColor : root.mutedTextColor
                        font.pixelSize: 7
                        font.weight: Font.Bold
                        font.letterSpacing: 0.6
                    }

                    Label {
                        text: root.beforeView ? "BEFORE · WARM PROXY" : "CURRENT · WARM PROXY"
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
                border.color: "#22282e"
                clip: true

                Canvas {
                    id: histogramCanvas

                    anchors.fill: parent
                    anchors.margins: 4
                    opacity: root.presentationStale ? 0.48 : root.updating ? 0.68 : 1.0
                    visible: root.hasData

                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const context = getContext("2d");
                        context.reset();
                        context.clearRect(0, 0, width, height);

                        context.lineWidth = 1;
                        context.strokeStyle = "rgba(78, 87, 96, 0.24)";
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
                        root.fillChannel(context, root.binList("red"), maximum, width, height,
                                         "rgba(235, 80, 76, 0.25)");
                        root.fillChannel(context, root.binList("green"), maximum, width, height,
                                         "rgba(86, 207, 118, 0.22)");
                        root.fillChannel(context, root.binList("blue"), maximum, width, height,
                                         "rgba(78, 135, 238, 0.27)");
                        root.strokeChannel(context, root.binList("luma"), maximum, width,
                                           height, "rgba(239, 243, 246, 0.92)");
                    }
                }

                Label {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 5
                    visible: root.hasData
                    text: "LOG"
                    color: "#66717c"
                    font.pixelSize: 6
                    font.weight: Font.Bold
                    font.letterSpacing: 0.5
                }

                Label {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 5
                    visible: root.hasData
                    text: root.sampleWidth + "×" + root.sampleHeight + (root.approximate
                                                                        ? " · APPROX" : "")
                    color: "#66717c"
                    font.pixelSize: 6
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.35
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: !root.hasData
                    text: root.updating ? "Analyzing the warm preview…" : "Histogram unavailable"
                    color: root.updating ? root.secondaryTextColor : root.mutedTextColor
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
                opacity: root.presentationStale ? 0.48 : root.updating ? 0.68 : 1.0

                Rectangle {
                    id: shadowClipBadge

                    Layout.fillWidth: true
                    Layout.preferredHeight: 21
                    radius: 3
                    color: root.hasData && root.shadowPixels > 0 ? "#1b2935" : "#161a1e"
                    border.color: root.hasData && root.shadowPixels > 0
                                  ? "#47657c" : root.borderColor

                    Label {
                        anchors.centerIn: parent
                        text: root.hasData
                            ? "◀  SHADOWS  " + root.clippedPercent(root.shadowFraction)
                            : "SHADOWS  —"
                        color: root.hasData && root.shadowPixels > 0
                            ? "#88b9dc" : root.mutedTextColor
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
                        ToolTip.text: "Any RGB channel below 0 before display clamp · "
                                      + root.pixelCountText(root.shadowPixels) + " proxy pixels"
                                      + root.channelClipText("belowZero")
                    }
                }

                Rectangle {
                    id: highlightClipBadge

                    Layout.fillWidth: true
                    Layout.preferredHeight: 21
                    radius: 3
                    color: root.hasData && root.highlightPixels > 0 ? "#332421" : "#161a1e"
                    border.color: root.hasData && root.highlightPixels > 0
                                  ? "#765048" : root.borderColor

                    Label {
                        anchors.centerIn: parent
                        text: root.hasData
                            ? "HIGHLIGHTS  " + root.clippedPercent(root.highlightFraction) + "  ▶"
                            : "HIGHLIGHTS  —"
                        color: root.hasData && root.highlightPixels > 0
                            ? "#dfa096" : root.mutedTextColor
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
                        ToolTip.text: "Any RGB channel above 1 before display clamp · "
                                      + root.pixelCountText(root.highlightPixels) + " proxy pixels"
                                      + root.channelClipText("aboveOne")
                    }
                }
            }
        }
    }
}
