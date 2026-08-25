pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls

// Display-only status and mode feedback above the Precision viewport. This component owns no
// image transport, view geometry, tool interaction, Recipe state, or persistence.
Item {
    id: overlays

    // Qt platform styles are allowed to render BusyIndicator very subtly.
    // Precision's foreground wait state instead uses one explicit accent dot
    // orbiting a stable ring, so a disclosed wait is visibly alive on every
    // supported desktop theme.
    component VisibleWaitSpinner: Item {
        id: spinner

        required property bool running

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: Theme.previewHudOverlay
        }

        Rectangle {
            id: spinnerRing
            anchors.centerIn: parent
            width: Math.max(9, Math.round(Math.min(parent.width,
                parent.height) * 0.72))
            height: width
            radius: width / 2
            color: Theme.transparent
            border.width: 1
            border.color: Theme.borderStrong
        }

        Item {
            id: spinnerRotor
            objectName: spinner.objectName.length > 0
                ? spinner.objectName + "Rotor" : ""
            anchors.centerIn: spinnerRing
            width: spinnerRing.width
            height: spinnerRing.height

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: -height / 2
                width: Math.max(3, Math.round(spinnerRing.width * 0.24))
                height: width
                radius: width / 2
                color: Theme.accent
            }

            NumberAnimation on rotation {
                from: 0
                to: 360
                duration: 720
                loops: Animation.Infinite
                running: spinner.running && spinner.visible
            }
        }
    }

    required property var editor
    required property bool comparisonActive
    required property int comparisonMode
    required property bool showingFullDetail
    required property bool fitView
    required property real zoomFactor
    required property bool detailImageReady
    required property bool detailImageLoadFailed
    required property bool beforeReady
    required property bool beforeFrameReady
    required property bool previewFrameReady
    required property bool previewLoadFailed
    required property int comparisonWhole
    required property int comparisonWipeVertical
    required property int comparisonWipeHorizontal
    required property int comparisonSideBySide

    property bool detailWaitMessageVisible: false
    property bool retainedPreviewBusyVisible: false
    property bool primaryWaitVisible: false

    // A ready frame may deliberately remain visible while its replacement is
    // rendering. Surface that unpublished-current-revision state instead of
    // making a responsive retained frame look like a non-responsive slider.
    readonly property bool retainedPreviewReplacementPending:
        previewFrameReady && editor.active
        && editor.rendering && !detailSurfaceRelevant

    readonly property bool detailSurfaceRelevant:
        !comparisonActive && !fitView && zoomFactor >= 1.0
    readonly property bool detailWaitActive:
        detailSurfaceRelevant && !detailImageReady
        && editor.detailErrorText.length === 0
        && !detailImageLoadFailed
        && (editor.detailMode || editor.detailRendering)
    readonly property bool primaryWaitActive:
        !previewFrameReady
        && (editor.stateBusy || editor.rendering
            || (comparisonActive && editor.beforeRendering))

    onDetailWaitActiveChanged: {
        if (!detailWaitActive)
            detailWaitMessageVisible = false
    }

    onRetainedPreviewReplacementPendingChanged: {
        if (!retainedPreviewReplacementPending)
            retainedPreviewBusyVisible = false
    }

    onPrimaryWaitActiveChanged: {
        if (!primaryWaitActive)
            primaryWaitVisible = false
    }

    Timer {
        interval: 250
        running: overlays.detailWaitActive
            && !overlays.detailWaitMessageVisible
        onTriggered: overlays.detailWaitMessageVisible =
            overlays.detailWaitActive
    }

    // Most retained-frame replacements settle within one or two display
    // updates. Do not flash progress chrome for that healthy fast path; only
    // disclose the still-pending revision once it is perceptible to a person.
    Timer {
        interval: 350
        running: overlays.retainedPreviewReplacementPending
            && !overlays.retainedPreviewBusyVisible
        onTriggered: overlays.retainedPreviewBusyVisible =
            overlays.retainedPreviewReplacementPending
    }

    Timer {
        interval: 250
        running: overlays.primaryWaitActive
            && !overlays.primaryWaitVisible
        onTriggered: overlays.primaryWaitVisible =
            overlays.primaryWaitActive
    }

    Rectangle {
        id: comparisonBadge
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 58
        anchors.rightMargin: 14
        width: comparisonBadgeLabel.implicitWidth + 18
        height: 25
        radius: 4
        visible: overlays.editor.active && overlays.comparisonActive
        color: Theme.previewHudOverlay
        border.color: Theme.accent

        Label {
            id: comparisonBadgeLabel
            anchors.centerIn: parent
            text: overlays.comparisonMode === overlays.comparisonWhole
                ? qsTranslate(
                    "PrecisionWorkspace", "BEFORE · NEUTRAL BASE")
                : overlays.comparisonMode === overlays.comparisonWipeVertical
                    ? qsTranslate(
                        "PrecisionWorkspace",
                        "BEFORE / AFTER · VERTICAL WIPE")
                    : overlays.comparisonMode === overlays.comparisonWipeHorizontal
                        ? qsTranslate(
                            "PrecisionWorkspace",
                            "BEFORE / AFTER · HORIZONTAL WIPE")
                        : overlays.comparisonMode === overlays.comparisonSideBySide
                            ? qsTranslate(
                                "PrecisionWorkspace",
                                "BEFORE / AFTER · SIDE BY SIDE")
                            : qsTranslate(
                                "PrecisionWorkspace",
                                "BEFORE / AFTER · TOP / BOTTOM")
            color: Theme.accent
            font.pixelSize: 8
            font.weight: Font.Bold
            font.letterSpacing: 0.7
        }
    }

    VisibleWaitSpinner {
        id: retainedPreviewBusyIndicator
        objectName: "precisionRetainedPreviewBusyIndicator"
        anchors.centerIn: parent
        width: 32
        height: 32
        visible: overlays.retainedPreviewBusyVisible
        running: visible
    }

    Rectangle {
        id: detailHint
        objectName: "precisionDetailWaitingHint"

        readonly property bool failed:
            overlays.editor.detailErrorText.length > 0
            || overlays.detailImageLoadFailed

        anchors.top: comparisonBadge.bottom
        anchors.right: comparisonBadge.right
        anchors.topMargin: 7
        width: failed || overlays.detailWaitMessageVisible
            ? Math.min(350, detailHintRow.implicitWidth + 20) : 30
        height: 30
        radius: 4
        visible: overlays.detailSurfaceRelevant
            && (overlays.detailWaitActive || failed)
        color: failed || overlays.detailWaitMessageVisible
            ? Theme.previewHudStrongOverlay : Theme.transparent
        border.width: failed || overlays.detailWaitMessageVisible ? 1 : 0
        border.color: failed ? Theme.errorBorder : Theme.border
        clip: true

        Behavior on width {
            NumberAnimation {
                duration: 120
                easing.type: Easing.OutCubic
            }
        }

        Row {
            id: detailHintRow
            anchors.centerIn: parent
            spacing: 7

            VisibleWaitSpinner {
                objectName: "precisionDetailWaitSpinner"
                width: 14
                height: 14
                visible: overlays.detailWaitActive
                running: visible
            }

            Label {
                objectName: "precisionDetailWaitingText"
                width: Math.min(290, implicitWidth)
                visible: detailHint.failed
                    || overlays.detailWaitMessageVisible
                text: detailHint.failed
                    ? (overlays.editor.detailErrorText.length > 0
                        ? overlays.editor.detailErrorText
                        : qsTranslate(
                            "PrecisionWorkspace",
                            "Full-detail viewport unavailable · showing proxy"))
                    : overlays.editor.fullResolutionPreparing
                        ? qsTranslate(
                            "PrecisionWorkspace",
                            "Preparing 100% detail…")
                        : overlays.editor.detailRendering
                            ? qsTranslate(
                                "PrecisionWorkspace",
                                "Rendering 100% detail…")
                            : qsTranslate(
                                "PrecisionWorkspace",
                                "Waiting for 100% detail…")
                color: detailHint.failed ? Theme.errorText : Theme.textMuted
                font.pixelSize: 9
                elide: Text.ElideRight
            }
        }
    }

    Rectangle {
        anchors.top: comparisonBadge.bottom
        anchors.right: comparisonBadge.right
        anchors.topMargin: 7
        width: Math.min(330, beforeHintRow.implicitWidth + 20)
        height: 30
        radius: 4
        visible: overlays.comparisonActive
            && (!overlays.beforeReady || !overlays.beforeFrameReady)
            && overlays.editor.active
        color: Theme.previewHudStrongOverlay
        border.color: Theme.border
        clip: true

        Row {
            id: beforeHintRow
            anchors.centerIn: parent
            spacing: 7

            VisibleWaitSpinner {
                width: 14
                height: 14
                visible: overlays.editor.beforeRendering
                running: visible
            }

            Label {
                width: Math.min(270, implicitWidth)
                text: overlays.editor.beforeErrorText.length > 0
                    ? overlays.editor.beforeErrorText
                    : overlays.editor.beforeRendering
                        ? qsTranslate(
                            "PrecisionWorkspace",
                            "Preparing neutral import baseline…")
                        : qsTranslate(
                            "PrecisionWorkspace",
                            "Waiting for the current preview…")
                color: overlays.editor.beforeErrorText.length > 0
                    ? Theme.errorText : Theme.textMuted
                font.pixelSize: 9
                elide: Text.ElideRight
            }
        }
    }

    Column {
        objectName: "precisionPrimaryWaitOverlay"
        anchors.centerIn: parent
        spacing: 14
        visible: overlays.primaryWaitVisible

        VisibleWaitSpinner {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 32
            height: 32
            running: parent.visible
        }

        Label {
            objectName: "precisionRenderingStatusText"
            text: overlays.editor.active
                ? qsTranslate(
                    "PrecisionWorkspace", "Rendering local edit")
                : qsTranslate("PrecisionWorkspace", "Opening photo")
            color: Theme.textPrimary
            font.pixelSize: 12
        }
    }

    Column {
        anchors.centerIn: parent
        width: Math.min(390, parent.width - 60)
        spacing: 10
        visible: !overlays.editor.busy
            && (!overlays.editor.active
                || (!overlays.previewFrameReady && overlays.previewLoadFailed))

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: overlays.previewLoadFailed
                ? qsTranslate("PrecisionWorkspace", "PREVIEW ERROR")
                : qsTranslate("PrecisionWorkspace", "NO PHOTO OPEN")
            color: overlays.previewLoadFailed
                ? Theme.errorText : Theme.textMuted
            font.pixelSize: 12
            font.weight: Font.DemiBold
            font.letterSpacing: 1.2
        }

        Label {
            width: parent.width
            text: overlays.editor.statusText
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pixelSize: 10
            lineHeight: 1.35
        }
    }
}
