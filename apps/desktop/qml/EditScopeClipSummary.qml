pragma ComponentBehavior: Bound
pragma Translator: "EditHistogram"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Presents the last complete histogram's pre-clamp shadow/highlight facts.
RowLayout {
    id: summary

    required property var dataModel
    required property color borderColor
    required property color mutedTextColor

    readonly property bool hasData: dataModel.hasHistogramData
    readonly property real shadowFraction: dataModel.shadowFraction
    readonly property real highlightFraction: dataModel.highlightFraction
    readonly property real shadowPixels: dataModel.shadowPixels
    readonly property real highlightPixels: dataModel.highlightPixels

    spacing: 5
    opacity: 1.0

    function clippedPercent(fraction) {
        const percent = Math.max(0, Number(fraction)) * 100
        if (!Number.isFinite(percent) || percent === 0)
            return qsTranslate("EditHistogram", "0%")
        if (percent < 0.01)
            return qsTranslate("EditHistogram", "<0.01%")
        return qsTranslate("EditHistogram", "%1%").arg(
            percent.toLocaleString(Qt.locale(), "f", percent < 1 ? 2 : 1))
    }

    function pixelCountText(value) {
        const count = Math.max(0, Math.round(Number(value)))
        if (!Number.isFinite(count))
            return "0"
        return count.toLocaleString(Qt.locale(), "f", 0)
    }

    function clipTooltip(name, pixelCount) {
        const counts = dataModel.binList(name)
        const formattedPixels = pixelCountText(pixelCount)
        if (name === "belowZero") {
            return counts.length === 3
                ? qsTranslate("EditHistogram", "Any RGB channel below 0 before display clamp · %1 proxy pixels\nR %2 · G %3 · B %4")
                    .arg(formattedPixels)
                    .arg(pixelCountText(counts[0]))
                    .arg(pixelCountText(counts[1]))
                    .arg(pixelCountText(counts[2]))
                : qsTranslate("EditHistogram", "Any RGB channel below 0 before display clamp · %1 proxy pixels")
                    .arg(formattedPixels)
        }
        return counts.length === 3
            ? qsTranslate("EditHistogram", "Any RGB channel above 1 before display clamp · %1 proxy pixels\nR %2 · G %3 · B %4")
                .arg(formattedPixels)
                .arg(pixelCountText(counts[0]))
                .arg(pixelCountText(counts[1]))
                .arg(pixelCountText(counts[2]))
            : qsTranslate("EditHistogram", "Any RGB channel above 1 before display clamp · %1 proxy pixels")
                .arg(formattedPixels)
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 21
        radius: 3
        color: summary.hasData && summary.shadowPixels > 0
            ? Theme.shadowClipSurface : Theme.clippingIdleSurface
        border.color: summary.hasData && summary.shadowPixels > 0
            ? Theme.shadowClipBorder : summary.borderColor

        Label {
            objectName: "analysisShadowSummaryText"
            anchors.centerIn: parent
            text: summary.hasData
                ? qsTranslate("EditHistogram", "◀  SHADOWS  %1").arg(
                    summary.clippedPercent(summary.shadowFraction))
                : qsTranslate("EditHistogram", "SHADOWS  —")
            color: summary.hasData && summary.shadowPixels > 0
                ? Theme.shadowClipText : summary.mutedTextColor
            font.pixelSize: 8
            font.weight: Font.Bold
            font.letterSpacing: 0.35
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            ToolTip.visible: containsMouse && summary.hasData
            ToolTip.delay: 450
            ToolTip.text: summary.clipTooltip(
                "belowZero", summary.shadowPixels)
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 21
        radius: 3
        color: summary.hasData && summary.highlightPixels > 0
            ? Theme.highlightClipSurface : Theme.clippingIdleSurface
        border.color: summary.hasData && summary.highlightPixels > 0
            ? Theme.highlightClipBorder : summary.borderColor

        Label {
            anchors.centerIn: parent
            text: summary.hasData
                ? qsTranslate("EditHistogram", "HIGHLIGHTS  %1  ▶").arg(
                    summary.clippedPercent(summary.highlightFraction))
                : qsTranslate("EditHistogram", "HIGHLIGHTS  —")
            color: summary.hasData && summary.highlightPixels > 0
                ? Theme.highlightClipText : summary.mutedTextColor
            font.pixelSize: 8
            font.weight: Font.Bold
            font.letterSpacing: 0.35
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            ToolTip.visible: containsMouse && summary.hasData
            ToolTip.delay: 450
            ToolTip.text: summary.clipTooltip(
                "aboveOne", summary.highlightPixels)
        }
    }
}
