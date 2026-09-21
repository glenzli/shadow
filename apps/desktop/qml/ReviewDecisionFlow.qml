pragma ComponentBehavior: Bound

import QtQuick

// A forward decision freezes one adjacent target. Completion can move selection
// only while that exact origin is still selected in the same browsing context.
QtObject {
    id: flow
    required property var controller
    required property var selection
    required property var navigationModel
    property bool autoAdvance: false
    property var pending: null
    signal advanceRequested(var target)

    onAutoAdvanceChanged: cancel()

    function cancel() { pending = null }

    function prepare() {
        cancel()
        if (!autoAdvance)
            return
        const next = navigationModel.navigationTarget(
            selection.selectedPhotoId, selection.selectedRepresentationId, 1, 0)
        if (next && String(next.photoId || "").length > 0)
            pending = { photoId: selection.selectedPhotoId,
                        representationId: selection.selectedRepresentationId,
                        target: next }
    }

    function setFlag(flag) {
        prepare()
        controller.setPhotoFlag(selection.selectedPhotoId, flag)
        Qt.callLater(clearIfIdle)
    }

    function setRating(rating) {
        prepare()
        controller.setPhotoRating(selection.selectedPhotoId, rating)
        Qt.callLater(clearIfIdle)
    }

    function clearIfIdle() {
        if (!controller.decisionBusy)
            cancel()
    }

    function committed(photoId) {
        const next = pending
        cancel()
        if (!autoAdvance || !next || next.photoId !== photoId
                || selection.selectedPhotoId !== next.photoId
                || selection.selectedRepresentationId !== next.representationId)
            return
        advanceRequested(next.target)
    }

    property Connections completion: Connections {
        target: flow.controller
        function onDecisionCommitted(photoId) { flow.committed(photoId) }
        function onDecisionStateChanged() { Qt.callLater(flow.clearIfIdle) }
        function onFiltersChanged() { flow.cancel() }
        function onLibraryOrderChanged() { flow.cancel() }
        function onLibraryAlbumChanged() { flow.cancel() }
    }
    property Connections selectionChanges: Connections {
        target: flow.selection
        function onSelectedPhotoIdChanged() { flow.cancel() }
        function onSelectedRepresentationIdChanged() { flow.cancel() }
    }
}
