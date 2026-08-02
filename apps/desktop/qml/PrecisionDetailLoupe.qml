pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A view-only consumer of the existing full-detail tile transport. The loupe
// owns no Recipe state and never keeps another full-resolution image in QML.
Rectangle {
    id: loupe
    objectName: "precisionDetailLoupe"

    required property var editor
    required property bool followPointer
    required property real zoomFactor
    required property real targetCenterX
    required property real targetCenterY
    required property real deviceScale
    required property real dragTopBoundary
    required property string targetKind
    required property bool focusConfirmed

    property real dragMargin: 8
    property bool positionInitialized: false
    property bool waitMessageVisible: false

    readonly property real detailViewportWidth: detailViewport.width
    readonly property real detailViewportHeight: detailViewport.height
    readonly property var detailTile: editor.detailTiles.length > 0
        ? editor.detailTiles[0] : ({})
    readonly property real detailTileX: finiteNumber(detailTile.x, 0)
    readonly property real detailTileY: finiteNumber(detailTile.y, 0)
    readonly property real detailTileWidth: Math.max(
        0, finiteNumber(detailTile.width, 0))
    readonly property real detailTileHeight: Math.max(
        0, finiteNumber(detailTile.height, 0))
    readonly property string detailSource: String(detailTile.source || "")
    readonly property real detailImageScale: Math.max(
        0.0001, zoomFactor / Math.max(1.0, deviceScale))
    readonly property real detailSourceCenterX: clamp01(targetCenterX)
        * Math.max(0, Number(editor.detailFullWidth || 0))
    readonly property real detailSourceCenterY: clamp01(targetCenterY)
        * Math.max(0, Number(editor.detailFullHeight || 0))
    readonly property real detailImageX: detailViewport.width / 2
        - (detailSourceCenterX - detailTileX) * detailImageScale
    readonly property real detailImageY: detailViewport.height / 2
        - (detailSourceCenterY - detailTileY) * detailImageScale
    readonly property real detailImageDisplayWidth:
        detailTileWidth * detailImageScale
    readonly property real detailImageDisplayHeight:
        detailTileHeight * detailImageScale
    readonly property real minimumWindowX: dragMargin
    readonly property real maximumWindowX: Math.max(
        minimumWindowX,
        (parent ? parent.width : width) - width - dragMargin)
    readonly property real minimumWindowY: Math.max(0, dragTopBoundary)
    readonly property real maximumWindowY: Math.max(
        minimumWindowY,
        (parent ? parent.height : height) - height - dragMargin)
    readonly property bool detailReady: detailImage.status === Image.Ready
    readonly property bool detailWaitActive:
        visible && !detailReady
        && String(editor.detailErrorText || "").length === 0
    readonly property string targetLabel: {
        if (targetKind === "camera")
            return focusConfirmed ? qsTr("Confirmed camera focus")
                                  : qsTr("Camera focus")
        if (targetKind === "manual")
            return qsTr("Manual position")
        return qsTr("Image center")
    }

    onDetailWaitActiveChanged: {
        if (!detailWaitActive)
            waitMessageVisible = false
    }

    Timer {
        interval: 250
        running: loupe.detailWaitActive && !loupe.waitMessageVisible
        onTriggered: loupe.waitMessageVisible = loupe.detailWaitActive
    }

    signal closeRequested()
    signal followPointerToggleRequested()
    signal zoomFactorRequested(real zoomFactor)

    function finiteNumber(value, fallback) {
        const number = Number(value)
        return isFinite(number) ? number : fallback
    }

    function clamp01(value) {
        return Math.max(0, Math.min(1, finiteNumber(value, 0.5)))
    }

    function keepInsideCanvas() {
        if (!parent)
            return
        x = boundedWindowX(x)
        y = boundedWindowY(y)
    }

    function boundedWindowX(value) {
        return Math.max(minimumWindowX, Math.min(maximumWindowX, value))
    }

    function boundedWindowY(value) {
        return Math.max(minimumWindowY, Math.min(maximumWindowY, value))
    }

    function resetWindowPosition() {
        if (!parent)
            return
        x = maximumWindowX
        y = minimumWindowY
        positionInitialized = true
    }

    width: 296
    height: 232
    radius: Theme.controlRadius + 2
    color: Theme.panelRaised
    border.width: 1
    border.color: Theme.borderStrong
    clip: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: Theme.chrome

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 6
                spacing: 6

                Item {
                    id: dragRegion
                    objectName: "detailLoupeDragRegion"
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: -1

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Detail loupe")
                            color: Theme.textPrimary
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: loupe.targetLabel
                            color: Theme.textMuted
                            font.pixelSize: 8
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: windowDrag
                        objectName: "detailLoupeDragHandle"
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: pressed
                            ? Qt.ClosedHandCursor : Qt.OpenHandCursor

                        property real pressWindowX: 0
                        property real pressWindowY: 0
                        property real pressPointerX: 0
                        property real pressPointerY: 0

                        onPressed: mouse => {
                            const point = dragRegion.mapToItem(
                                loupe.parent, mouse.x, mouse.y)
                            pressWindowX = loupe.x
                            pressWindowY = loupe.y
                            pressPointerX = point.x
                            pressPointerY = point.y
                        }

                        onPositionChanged: mouse => {
                            if (!pressed || !loupe.parent)
                                return
                            const point = dragRegion.mapToItem(
                                loupe.parent, mouse.x, mouse.y)
                            loupe.x = loupe.boundedWindowX(
                                pressWindowX + point.x - pressPointerX)
                            loupe.y = loupe.boundedWindowY(
                                pressWindowY + point.y - pressPointerY)
                        }

                        onReleased: loupe.keepInsideCanvas()
                        onCanceled: loupe.keepInsideCanvas()
                    }
                }

                ShadowIconButton {
                    id: followButton
                    objectName: "detailLoupeFollowButton"
                    source: "qrc:/icons/pin.svg"
                    buttonSize: 26
                    iconSize: 14
                    variant: ShadowIconButton.Secondary
                    selected: !loupe.followPointer
                    toolTipText: loupe.followPointer
                        ? qsTr("Pin the current detail position")
                        : qsTr("Follow the pointer over the photo")
                    accessibleName: toolTipText
                    Accessible.checked: selected
                    onClicked: loupe.followPointerToggleRequested()
                }

                ShadowIconButton {
                    id: closeButton
                    objectName: "detailLoupeCloseButton"
                    source: "qrc:/icons/close.svg"
                    buttonSize: 26
                    iconSize: 12
                    variant: ShadowIconButton.Secondary
                    toolTipText: qsTr("Close detail loupe")
                    accessibleName: toolTipText
                    onClicked: loupe.closeRequested()
                }
            }
        }

        Item {
            id: detailViewport
            objectName: "detailLoupeViewport"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 1
            clip: true

            Rectangle {
                anchors.fill: parent
                color: Theme.photoCanvas
            }

            Image {
                id: detailImage
                objectName: "detailLoupeImage"
                x: loupe.detailImageX
                y: loupe.detailImageY
                width: loupe.detailImageDisplayWidth
                height: loupe.detailImageDisplayHeight
                source: loupe.detailSource
                fillMode: Image.Stretch
                asynchronous: true
                cache: false
                retainWhileLoading: true
                smooth: false
                mipmap: false
                opacity: loupe.detailWaitActive ? 0.78 : 1.0

                Behavior on opacity {
                    NumberAnimation { duration: 120 }
                }
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                width: 15
                height: 1
                color: Qt.rgba(1, 1, 1, 0.78)
                border.width: 0
                visible: loupe.detailReady
            }

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                width: 1
                height: 15
                color: Qt.rgba(1, 1, 1, 0.78)
                border.width: 0
                visible: loupe.detailReady
            }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(244, parent.width - 20)
                height: 58
                radius: 4
                visible: loupe.detailWaitActive
                    && loupe.waitMessageVisible
                color: Theme.previewHudStrongOverlay
                border.width: 1
                border.color: Theme.border
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(230, parent.width - 28)
                spacing: 6
                visible: loupe.detailWaitActive

                BusyIndicator {
                    objectName: "detailLoupeBusyIndicator"
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 30
                    height: 30
                    running: loupe.detailWaitActive
                }

                Label {
                    objectName: "detailLoupeWaitText"
                    width: parent.width
                    visible: loupe.waitMessageVisible
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: loupe.editor.fullResolutionPreparing
                        ? qsTr("Preparing 100% detail…")
                        : loupe.editor.detailRendering
                            ? qsTr("Rendering 100% detail…")
                            : qsTr("Waiting for 100% detail…")
                    color: Theme.textPrimary
                    font.pixelSize: 9
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 28
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: String(loupe.editor.detailErrorText || "")
                color: Theme.labelRed
                font.pixelSize: 9
                visible: text.length > 0 && !loupe.detailReady
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.chrome

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 4

                Label {
                    Layout.fillWidth: true
                    text: loupe.followPointer
                        ? qsTr("Following pointer")
                        : qsTr("Pinned · click photo to reposition")
                    color: Theme.textMuted
                    font.pixelSize: 8
                    elide: Text.ElideRight
                }

                ShadowButton {
                    objectName: "detailLoupe100Button"
                    compact: true
                    variant: ShadowButton.Secondary
                    selected: Math.abs(loupe.zoomFactor - 1.0) < 0.001
                    text: qsTr("100%")
                    onClicked: loupe.zoomFactorRequested(1.0)
                }

                ShadowButton {
                    objectName: "detailLoupe200Button"
                    compact: true
                    variant: ShadowButton.Secondary
                    selected: Math.abs(loupe.zoomFactor - 2.0) < 0.001
                    text: qsTr("200%")
                    onClicked: loupe.zoomFactorRequested(2.0)
                }
            }
        }
    }

    Component.onCompleted: Qt.callLater(resetWindowPosition)

    onVisibleChanged: {
        if (visible && !positionInitialized)
            Qt.callLater(resetWindowPosition)
    }

    Connections {
        target: loupe.parent

        function onWidthChanged() {
            if (loupe.positionInitialized)
                loupe.keepInsideCanvas()
        }

        function onHeightChanged() {
            if (loupe.positionInitialized)
                loupe.keepInsideCanvas()
        }
    }
}
