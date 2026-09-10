pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls

// Owns the complete visual comparison transaction inside the precision photo
// surface: original-frame receipt, whole/wipe/dual layouts, divider input, and
// BEFORE/AFTER labels. The parent owns only the selected mode and position.
Item {
    id: comparison

    required property var editor
    required property var editPreviewPresentation
    required property bool comparisonActive
    required property int comparisonMode
    required property real comparisonPosition
    required property bool beforeReady
    required property string afterPreviewSource
    required property int wipeVerticalMode
    required property int wipeHorizontalMode
    required property int sideBySideMode
    required property int stackedMode

    readonly property bool dualComparison: comparisonActive && beforeReady
        && (comparisonMode === sideBySideMode
            || comparisonMode === stackedMode)
    property bool beforeFrameReady: false
    required property real imageDisplayWidth
    required property real imageDisplayHeight
    required property real imageCenterX
    required property real imageCenterY

    signal comparisonPositionRequested(real nextPosition)

    function updateComparisonPosition(sourceItem, sourceX, sourceY) {
        const mapped = sourceItem.mapToItem(
            comparison, sourceX, sourceY)
        if (comparisonMode === wipeVerticalMode) {
            comparisonPositionRequested(Math.max(0.02, Math.min(
                0.98, mapped.x / Math.max(1, width))))
        } else if (comparisonMode === wipeHorizontalMode) {
            comparisonPositionRequested(Math.max(0.02, Math.min(
                0.98, mapped.y / Math.max(1, height))))
        }
    }

    Item {
        id: beforeClip
        x: 0
        y: 0
        width: comparison.comparisonMode === comparison.wipeVerticalMode
            ? comparison.width * comparison.comparisonPosition
            : comparison.width
        height: comparison.comparisonMode === comparison.wipeHorizontalMode
            ? comparison.height * comparison.comparisonPosition
            : comparison.height
        clip: true
        visible: comparison.comparisonActive && comparison.beforeReady

        Image {
            id: beforePreviewImage
            x: 0
            y: 0
            width: comparison.width
            height: comparison.height
            source: comparison.editor.beforePreviewSource
            fillMode: Image.Stretch
            asynchronous: true
            cache: false
            retainWhileLoading: true
            smooth: true
            onSourceChanged: comparison.beforeFrameReady = false
            onStatusChanged: {
                if (status === Image.Ready)
                    comparison.beforeFrameReady = true
                else if (status === Image.Null || status === Image.Error)
                    comparison.beforeFrameReady = false
            }
        }
    }

    Rectangle {
        id: dualCompareSurface
        anchors.fill: parent
        visible: comparison.dualComparison
        color: Theme.photoCanvas
        z: 10

        Item {
            id: dualBeforePane
            x: 0
            y: 0
            width: comparison.comparisonMode === comparison.sideBySideMode
                ? parent.width / 2 : parent.width
            height: comparison.comparisonMode === comparison.stackedMode
                ? parent.height / 2 : parent.height
            clip: true

            Image {
                x: parent.width / 2 - comparison.imageCenterX * width
                y: parent.height / 2 - comparison.imageCenterY * height
                width: comparison.imageDisplayWidth
                height: comparison.imageDisplayHeight
                source: comparison.editor.beforePreviewSource
                fillMode: Image.Stretch
                asynchronous: true
                cache: false
                retainWhileLoading: true
                smooth: true
            }

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 14
                width: dualBeforeLabel.implicitWidth + 14
                height: 23
                radius: 4
                color: Theme.previewHudStrongOverlay
                border.color: Theme.previewHudBorder

                Label {
                    id: dualBeforeLabel
                    anchors.centerIn: parent
                    text: qsTr("BEFORE")
                    color: Theme.textSecondary
                    font.pixelSize: 8
                    font.weight: Font.Bold
                    font.letterSpacing: 0.7
                }
            }
        }

        Item {
            id: dualAfterPane
            x: comparison.comparisonMode === comparison.sideBySideMode
                ? parent.width / 2 : 0
            y: comparison.comparisonMode === comparison.stackedMode
                ? parent.height / 2 : 0
            width: comparison.comparisonMode === comparison.sideBySideMode
                ? parent.width / 2 : parent.width
            height: comparison.comparisonMode === comparison.stackedMode
                ? parent.height / 2 : parent.height
            clip: true

            Image {
                x: parent.width / 2 - comparison.imageCenterX * width
                y: parent.height / 2 - comparison.imageCenterY * height
                width: comparison.imageDisplayWidth
                height: comparison.imageDisplayHeight
                source: dualAfterLivePreview.fallbackSource
                fillMode: Image.Stretch
                asynchronous: true
                cache: false
                retainWhileLoading: true
                smooth: true
            }

            EditPreviewTextureItem {
                id: dualAfterLivePreview
                objectName: "dualAfterLivePreview"
                x: parent.width / 2 - comparison.imageCenterX * width
                y: parent.height / 2 - comparison.imageCenterY * height
                width: comparison.imageDisplayWidth
                height: comparison.imageDisplayHeight
                presentationRegistry:
                    comparison.editPreviewPresentation
                source: comparison.afterPreviewSource
                liveAdmissionEnabled: comparison.dualComparison
                fillMode: EditPreviewTextureItem.Stretch
            }

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 14
                width: dualAfterLabel.implicitWidth + 14
                height: 23
                radius: 4
                color: Theme.previewHudStrongOverlay
                border.color: Theme.previewHudBorder

                Label {
                    id: dualAfterLabel
                    anchors.centerIn: parent
                    text: qsTr("AFTER")
                    color: Theme.textSecondary
                    font.pixelSize: 8
                    font.weight: Font.Bold
                    font.letterSpacing: 0.7
                }
            }
        }

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 1
            height: parent.height
            visible: comparison.comparisonMode === comparison.sideBySideMode
            color: Theme.previewHudBorder
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 1
            visible: comparison.comparisonMode === comparison.stackedMode
            color: Theme.previewHudBorder
        }
    }

    Rectangle {
        id: verticalComparisonDivider
        x: Math.round(comparison.width * comparison.comparisonPosition)
        y: 0
        width: 1
        height: comparison.height
        visible: comparison.comparisonActive
            && comparison.beforeReady
            && comparison.comparisonMode === comparison.wipeVerticalMode
        color: Theme.previewCompareDivider
        z: 60

        Rectangle {
            anchors.centerIn: parent
            width: 18
            height: 32
            radius: 9
            color: Theme.previewHudStrongOverlay
            border.width: 1
            border.color: Theme.previewCompareDivider

            Rectangle {
                anchors.centerIn: parent
                width: 2
                height: 14
                radius: 1
                color: Theme.previewCompareDivider
            }
        }

        MouseArea {
            id: verticalDividerDragArea
            x: -12
            y: 0
            width: 25
            height: parent.height
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeHorCursor
            onPressed: mouse => comparison.updateComparisonPosition(
                verticalDividerDragArea, mouse.x, mouse.y)
            onPositionChanged: mouse => {
                if (pressed)
                    comparison.updateComparisonPosition(
                        verticalDividerDragArea, mouse.x, mouse.y)
            }
        }
    }

    Rectangle {
        id: horizontalComparisonDivider
        x: 0
        y: Math.round(comparison.height * comparison.comparisonPosition)
        width: comparison.width
        height: 1
        visible: comparison.comparisonActive
            && comparison.beforeReady
            && comparison.comparisonMode === comparison.wipeHorizontalMode
        color: Theme.previewCompareDivider
        z: 60

        Rectangle {
            anchors.centerIn: parent
            width: 32
            height: 18
            radius: 9
            color: Theme.previewHudStrongOverlay
            border.width: 1
            border.color: Theme.previewCompareDivider

            Rectangle {
                anchors.centerIn: parent
                width: 14
                height: 2
                radius: 1
                color: Theme.previewCompareDivider
            }
        }

        MouseArea {
            id: horizontalDividerDragArea
            x: 0
            y: -12
            width: parent.width
            height: 25
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeVerCursor
            onPressed: mouse => comparison.updateComparisonPosition(
                horizontalDividerDragArea, mouse.x, mouse.y)
            onPositionChanged: mouse => {
                if (pressed)
                    comparison.updateComparisonPosition(
                        horizontalDividerDragArea, mouse.x, mouse.y)
            }
        }
    }

    Rectangle {
        id: wipeBeforeBadge
        x: 12
        y: 12
        width: wipeBeforeLabel.implicitWidth + 14
        height: 23
        radius: 4
        visible: comparison.comparisonActive
            && comparison.beforeReady
            && (comparison.comparisonMode === comparison.wipeVerticalMode
                || comparison.comparisonMode === comparison.wipeHorizontalMode)
        color: Theme.previewHudStrongOverlay
        border.color: Theme.previewHudBorder
        z: 70

        Label {
            id: wipeBeforeLabel
            anchors.centerIn: parent
            text: qsTr("BEFORE")
            color: Theme.textSecondary
            font.pixelSize: 8
            font.weight: Font.Bold
            font.letterSpacing: 0.7
        }
    }

    Rectangle {
        id: wipeAfterBadge
        x: comparison.comparisonMode === comparison.wipeVerticalMode
            ? comparison.width - width - 12 : 12
        y: comparison.comparisonMode === comparison.wipeHorizontalMode
            ? comparison.height - height - 12 : 12
        width: wipeAfterLabel.implicitWidth + 14
        height: 23
        radius: 4
        visible: wipeBeforeBadge.visible
        color: Theme.previewHudStrongOverlay
        border.color: Theme.previewHudBorder
        z: 70

        Label {
            id: wipeAfterLabel
            anchors.centerIn: parent
            text: qsTr("AFTER")
            color: Theme.textSecondary
            font.pixelSize: 8
            font.weight: Font.Bold
            font.letterSpacing: 0.7
        }
    }
}
