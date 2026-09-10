pragma ComponentBehavior: Bound

import QtQuick

// Atomically replaces a viewport's pixels and source-coordinate rectangle.
// The visible slot keeps its own geometry while the next image is loading.
Item {
    id: surface

    required property var tile
    required property real displayScale
    property int activeSlot: -1
    property int pendingSlot: -1
    property string requestedSource: ""
    property bool loadFailed: false
    property bool initialized: false
    readonly property bool ready: activeSlot >= 0

    function stage() {
        const nextSource = String(tile.source || "")
        if (nextSource === requestedSource)
            return
        requestedSource = nextSource
        loadFailed = false
        if (nextSource.length === 0) {
            activeSlot = -1
            pendingSlot = -1
            first.tileSnapshot = ({})
            second.tileSnapshot = ({})
            return
        }
        pendingSlot = activeSlot === 0 ? 1 : 0
        const slot = pendingSlot === 0 ? first : second
        slot.tileSnapshot = tile
        acceptReady(slot)
    }

    function acceptReady(slot) {
        if (pendingSlot !== slot.slotIndex
                || String(slot.tileSnapshot.source || "") !== requestedSource)
            return
        if (slot.status === Image.Ready) {
            activeSlot = pendingSlot
            pendingSlot = -1
        } else if (slot.status === Image.Error) {
            loadFailed = true
        }
    }

    onTileChanged: if (initialized) stage()
    Component.onCompleted: {
        initialized = true
        stage()
    }

    component DetailImage: Image {
        required property int slotIndex
        property var tileSnapshot: ({})
        x: Number(tileSnapshot.x || 0) * surface.displayScale
        y: Number(tileSnapshot.y || 0) * surface.displayScale
        width: Number(tileSnapshot.width || 0) * surface.displayScale
        height: Number(tileSnapshot.height || 0) * surface.displayScale
        source: String(tileSnapshot.source || "")
        fillMode: Image.Stretch
        asynchronous: true
        cache: false
        smooth: false
        visible: surface.activeSlot === slotIndex
        onStatusChanged: surface.acceptReady(this)
    }

    DetailImage { id: first; slotIndex: 0; objectName: "firstDetailSlot" }
    DetailImage { id: second; slotIndex: 1; objectName: "secondDetailSlot" }
}
