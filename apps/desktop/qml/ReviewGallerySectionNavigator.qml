pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls

// Owns the transient reveal, sampling, preview, and pointer lifecycle for
// jumping between concrete grouped-gallery section rows. Group semantics and
// row geometry stay in their native owners.
Item {
    id: navigator

    required property var view
    required property var sectionAnchors
    required property bool groupingActive

    property bool revealed: false
    property bool dragging: false
    property int currentIndex: 0
    property int interactionIndex: currentIndex
    property bool initialized: false

    readonly property bool eligible: groupingActive
        && view.visible
        && sectionAnchors.length >= 3
        && view.contentHeight > view.height * 1.25
    readonly property int labelStride: Math.max(1,
        Math.ceil(sectionAnchors.length / 10))

    objectName: "reviewGallerySectionNavigator"
    width: 52
    height: Math.min(360, Math.max(168, view.height * 0.64))
    visible: eligible
    opacity: revealed || scrubArea.containsMouse || dragging || activeFocus ? 1 : 0
    activeFocusOnTab: eligible
    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Group navigator")

    Behavior on opacity {
        NumberAnimation { duration: 170; easing.type: Easing.OutCubic }
    }

    function anchorAt(index) {
        if (index < 0 || index >= sectionAnchors.length)
            return null
        return sectionAnchors[index]
    }

    function markerY(index) {
        if (sectionAnchors.length <= 1)
            return height / 2
        return 14 + index * (height - 28) / (sectionAnchors.length - 1)
    }

    function indexForY(localY) {
        if (sectionAnchors.length <= 1)
            return 0
        const fraction = Math.max(0, Math.min(1,
            (localY - 14) / Math.max(1, height - 28)))
        return Math.round(fraction * (sectionAnchors.length - 1))
    }

    function labelFor(index) {
        const anchor = anchorAt(index)
        if (!anchor)
            return ""
        const shortLabel = String(anchor.navigationShortLabel || "")
        return shortLabel.length > 0 ? shortLabel
            : String(anchor.navigationLabel || anchor.title || "")
    }

    function shouldShowLabel(index) {
        return index === 0 || index === sectionAnchors.length - 1
            || index === currentIndex || index === interactionIndex
            || index % labelStride === 0
    }

    function reveal() {
        if (!eligible)
            return
        revealed = true
        dismissTimer.restart()
    }

    function updateCurrentFromView() {
        if (!eligible || sectionAnchors.length === 0)
            return
        let visibleRow = view.indexAt(1, Math.max(0, view.contentY + 2))
        if (visibleRow < 0)
            visibleRow = view.contentY <= 0 ? 0 : view.count - 1
        let next = 0
        for (let index = 1; index < sectionAnchors.length; ++index) {
            if (Number(sectionAnchors[index].rowIndex) > visibleRow)
                break
            next = index
        }
        currentIndex = next
        if (!dragging)
            interactionIndex = next
    }

    function jumpTo(index) {
        const bounded = Math.max(0, Math.min(sectionAnchors.length - 1, index))
        const anchor = anchorAt(bounded)
        if (!anchor)
            return
        interactionIndex = bounded
        currentIndex = bounded
        view.positionViewAtIndex(Number(anchor.rowIndex), ListView.Beginning)
        view.forceActiveFocus()
        reveal()
    }

    onEligibleChanged: {
        if (!eligible) {
            dismissTimer.stop()
            revealed = false
            dragging = false
        } else {
            updateCurrentFromView()
        }
    }
    onActiveFocusChanged: {
        if (activeFocus)
            reveal()
    }
    onSectionAnchorsChanged: {
        currentIndex = 0
        interactionIndex = 0
        Qt.callLater(updateCurrentFromView)
    }
    Component.onCompleted: Qt.callLater(function() {
        initialized = true
        updateCurrentFromView()
    })

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Up)
            jumpTo(interactionIndex - 1)
        else if (event.key === Qt.Key_Down)
            jumpTo(interactionIndex + 1)
        else if (event.key === Qt.Key_Home)
            jumpTo(0)
        else if (event.key === Qt.Key_End)
            jumpTo(sectionAnchors.length - 1)
        else
            return
        event.accepted = true
    }

    Connections {
        target: navigator.view

        function onContentYChanged() {
            navigator.updateCurrentFromView()
            if (navigator.initialized)
                navigator.reveal()
        }

        function onMovingChanged() {
            if (navigator.view.moving)
                navigator.reveal()
            else if (!navigator.dragging)
                dismissTimer.restart()
        }
    }

    Timer {
        id: dismissTimer
        interval: 1800
        repeat: false
        onTriggered: {
            if (!navigator.dragging && !scrubArea.containsMouse)
                navigator.revealed = false
        }
    }

    Rectangle {
        id: navigatorSurface
        anchors.fill: parent
        radius: 12
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.border
        opacity: 0.94
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: 12
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: 13
        anchors.bottomMargin: 13
        width: 1
        color: Theme.borderStrong
    }

    Repeater {
        model: navigator.sectionAnchors

        delegate: Item {
            id: marker
            required property int index
            required property var modelData

            x: 0
            y: navigator.markerY(index) - height / 2
            width: navigator.width
            height: 18

            readonly property bool active: index === navigator.currentIndex
                || navigator.dragging && index === navigator.interactionIndex

            Label {
                anchors.right: parent.right
                anchors.rightMargin: 17
                anchors.verticalCenter: parent.verticalCenter
                width: 30
                text: navigator.labelFor(marker.index)
                visible: navigator.shouldShowLabel(marker.index)
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                color: marker.active ? Theme.accentTextMuted : Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: marker.active ? Font.DemiBold : Font.Medium
            }

            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: marker.active ? 7 : 3
                height: marker.active ? 7 : 3
                radius: width / 2
                color: marker.active ? Theme.accent : Theme.textFaint

                Behavior on width { NumberAnimation { duration: 100 } }
                Behavior on height { NumberAnimation { duration: 100 } }
            }
        }
    }

    Rectangle {
        id: previewBubble
        objectName: "reviewGallerySectionNavigatorPreview"
        anchors.right: parent.left
        anchors.rightMargin: 8
        y: Math.max(0, Math.min(navigator.height - height,
            navigator.markerY(navigator.interactionIndex) - height / 2))
        width: 154
        height: previewLabel.implicitHeight + 18
        radius: 8
        visible: navigator.dragging
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong

        Label {
            id: previewLabel
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            text: {
                const anchor = navigator.anchorAt(navigator.interactionIndex)
                return anchor ? String(anchor.title || anchor.navigationLabel || "") : ""
            }
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            font.weight: Font.Medium
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
    }

    MouseArea {
        id: scrubArea
        objectName: "reviewGallerySectionNavigatorInput"
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: navigator.revealed || pressed ? navigator.width : 9
        hoverEnabled: true
        preventStealing: true
        cursorShape: navigator.revealed ? Qt.PointingHandCursor : Qt.ArrowCursor

        onEntered: navigator.reveal()
        onExited: {
            if (!pressed)
                dismissTimer.restart()
        }
        onPressed: mouse => {
            navigator.forceActiveFocus()
            navigator.dragging = true
            navigator.reveal()
            navigator.jumpTo(navigator.indexForY(mouse.y))
        }
        onPositionChanged: mouse => {
            if (pressed)
                navigator.jumpTo(navigator.indexForY(mouse.y))
        }
        onReleased: {
            navigator.dragging = false
            dismissTimer.restart()
        }
        onCanceled: {
            navigator.dragging = false
            dismissTimer.restart()
        }
    }
}
