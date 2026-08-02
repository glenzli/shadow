pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the compact, retryable presentation of the background GPS-to-place
// lifecycle. Both Library filter surfaces consume the same controller state.
Rectangle {
    id: root

    required property var controller

    readonly property string statusCode:
        String(controller.libraryPlaceResolutionStatusCode || "idle")
    readonly property bool actionableFailure:
        statusCode === "failed" || statusCode === "partial"
    readonly property bool shouldPresent:
        statusCode === "permission-required"
        || statusCode === "resolving"
        || actionableFailure
        || (statusCode === "complete"
            && Number(controller.libraryPlaceResolutionRecordedCount) > 0)

    function statusText() {
        const processed = Number(controller.libraryPlaceResolutionProcessedCount)
        const recorded = Number(controller.libraryPlaceResolutionRecordedCount)
        const failed = Number(controller.libraryPlaceResolutionFailedCount)
        if (statusCode === "permission-required")
            return qsTr("City lookup is unavailable. Check the offline location data in this Shadow installation.")
        if (statusCode === "resolving")
            return qsTr("Resolving photo cities offline… %L1 processed").arg(processed)
        if (statusCode === "partial")
            return qsTr("Resolved %L1 locations; %L2 could not be resolved.").arg(recorded).arg(failed)
        if (statusCode === "failed") {
            const detail = String(controller.libraryPlaceResolutionErrorText || "")
            return detail.length > 0
                ? qsTr("Could not resolve photo locations: %1").arg(detail)
                : qsTr("Could not resolve photo locations.")
        }
        return qsTr("Resolved %L1 photo locations.").arg(recorded)
    }

    Layout.fillWidth: true
    Layout.preferredHeight: shouldPresent ? statusRow.implicitHeight + 14 : 0
    visible: shouldPresent
    radius: Theme.compactControlRadius
    color: actionableFailure ? Theme.dangerSurface : Theme.surfaceSubtle
    border.width: 1
    border.color: actionableFailure ? Theme.errorBorder : Theme.border

    RowLayout {
        id: statusRow
        anchors.fill: parent
        anchors.margins: 7
        spacing: 8

        BusyIndicator {
            Layout.preferredWidth: 14
            Layout.preferredHeight: 14
            visible: root.statusCode === "resolving"
            running: visible
        }

        Label {
            Layout.fillWidth: true
            text: root.statusText()
            color: root.actionableFailure ? Theme.errorText : Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        ShadowButton {
            compact: true
            visible: root.actionableFailure
            text: qsTr("Retry")
            onClicked: root.controller.retryLibraryPlaceResolution()
        }
    }
}
