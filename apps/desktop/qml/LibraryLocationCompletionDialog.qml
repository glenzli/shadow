pragma ComponentBehavior: Bound
pragma Translator: LibraryLocationCompletionDialog

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window

// A read-only event browser for photos whose current effective metadata has
// no coordinates. Applying a group is deliberately delegated to the existing
// coordinate-batch preview dialog, so this surface can never write silently.
Window {
    id: root
    objectName: "locationCompletionDialog"

    required property var controller
    signal useGroupRequested(var targets, bool hasSuggestion,
                             real latitude, real longitude, string placeName)

    title: qsTr("Complete Photo Locations")
    width: 840
    height: 660
    minimumWidth: 660
    minimumHeight: 480
    color: Theme.window
    flags: Qt.Dialog
    modality: Qt.WindowModal

    property string startDay: ""
    property string endDay: ""

    function timestampForDay(text, endOfDay) {
        const matched = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(text).trim())
        if (!matched)
            return 0
        const value = new Date(Number(matched[1]), Number(matched[2]) - 1,
                               Number(matched[3]), endOfDay ? 23 : 0,
                               endOfDay ? 59 : 0, endOfDay ? 59 : 0).getTime()
        return Number.isFinite(value) ? Math.floor(value / 1000) : 0
    }

    function formatMoment(seconds) {
        if (Number(seconds) <= 0)
            return qsTr("Unknown time")
        return new Date(Number(seconds) * 1000).toLocaleString(
                    Qt.locale(), Locale.ShortFormat)
    }

    function refresh() {
        const start = timestampForDay(startDay, false)
        const end = timestampForDay(endDay, true)
        if ((startDay.trim().length > 0 && start === 0)
                || (endDay.trim().length > 0 && end === 0)
                || (start > 0 && end > 0 && start > end))
            return false
        controller.requestLocationCompletion(start, end)
        return true
    }

    function present() {
        controller.refreshLocationReferenceLibraries()
        show()
        raise()
        requestActivate()
        return refresh()
    }

    FolderDialog {
        id: referenceFolderDialog
        title: qsTr("Choose a reference photo folder")
        onAccepted: root.controller.addLocationReferenceLibrary(selectedFolder, 0)
    }

    onClosing: close => {
        close.accepted = !controller.locationCompletionBusy
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: root.visible && !root.controller.locationCompletionBusy
        onActivated: root.close()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 14

        RowLayout {
            Layout.fillWidth: true

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3

                Label {
                    text: qsTr("Complete locations")
                    color: Theme.textPrimary
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Review time-based events before assigning a location. Original EXIF remains unchanged.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }
            }

            ShadowIconButton {
                source: "qrc:/icons/close.svg"
                buttonSize: 32
                iconSize: 16
                enabled: !root.controller.locationCompletionBusy
                toolTipText: qsTr("Close")
                accessibleName: toolTipText
                onClicked: root.close()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: referenceContent.implicitHeight + 24
            radius: 8
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.border

            ColumnLayout {
                id: referenceContent
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: qsTr("Reference photo folders")
                            color: Theme.textPrimary
                            font.weight: Font.DemiBold
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Read only capture time and GPS from a phone or camera folder. These photos are not imported into your Library.")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.WordWrap
                        }
                    }

                    Button {
                        id: referenceAddButton
                        objectName: "locationReferenceAddButton"
                        text: qsTr("Add folder")
                        enabled: !root.controller.locationReferenceBusy
                        onClicked: referenceFolderDialog.open()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.controller.locationReferenceErrorText.length > 0
                    text: root.controller.locationReferenceErrorText
                    color: Theme.dangerText
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.controller.locationReferenceLibraries.length === 0
                        && !root.controller.locationReferenceBusy
                    text: qsTr("No reference folder added")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }

                Repeater {
                    id: referenceList
                    objectName: "locationReferenceList"
                    model: root.controller.locationReferenceLibraries

                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: referenceRow.implicitHeight + 12
                        radius: 6
                        color: Theme.panel

                        RowLayout {
                            id: referenceRow
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 8

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                Label {
                                    Layout.fillWidth: true
                                    text: String(modelData.rootPath || "")
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontMeta
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    text: qsTr("%L1 GPS anchors").arg(Number(modelData.anchorCount))
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.fontMeta
                                }
                            }

                            Label {
                                text: qsTr("Clock offset")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontMeta
                            }
                            SpinBox {
                                id: offsetBox
                                from: -43200
                                to: 43200
                                stepSize: 60
                                editable: true
                                value: Number(modelData.clockOffsetSeconds)
                                enabled: !root.controller.locationReferenceBusy
                                Accessible.name: qsTr("Clock offset seconds")
                            }
                            Button {
                                text: qsTr("Rescan")
                                enabled: !root.controller.locationReferenceBusy
                                onClicked: root.controller.addLocationReferenceLibrary(
                                    modelData.rootUrl, offsetBox.value)
                            }
                            Button {
                                text: qsTr("Remove")
                                enabled: !root.controller.locationReferenceBusy
                                onClicked: root.controller.removeLocationReferenceLibrary(
                                    String(modelData.id || ""))
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("Capture range")
                color: Theme.textSecondary
                font.weight: Font.DemiBold
            }

            TextField {
                id: startField
                objectName: "locationCompletionStartField"
                Layout.preferredWidth: 128
                placeholderText: qsTr("Start YYYY-MM-DD")
                text: root.startDay
                enabled: !root.controller.locationCompletionBusy
                onEditingFinished: root.startDay = text.trim()
            }

            Label {
                text: qsTr("to")
                color: Theme.textMuted
            }

            TextField {
                id: endField
                objectName: "locationCompletionEndField"
                Layout.preferredWidth: 128
                placeholderText: qsTr("End YYYY-MM-DD")
                text: root.endDay
                enabled: !root.controller.locationCompletionBusy
                onEditingFinished: root.endDay = text.trim()
            }

            Button {
                objectName: "locationCompletionRefreshButton"
                text: qsTr("Refresh")
                enabled: !root.controller.locationCompletionBusy
                onClicked: {
                    root.startDay = startField.text.trim()
                    root.endDay = endField.text.trim()
                    root.refresh()
                }
            }

            Item { Layout.fillWidth: true }

            Label {
                visible: root.controller.locationCompletionBusy
                text: qsTr("Finding events…")
                color: Theme.textMuted
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.startDay.trim().length > 0 || root.endDay.trim().length > 0
            text: qsTr("Only photos inside this capture range are considered.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
        }

        Label {
            Layout.fillWidth: true
            visible: root.controller.locationCompletionErrorText.length > 0
            text: root.controller.locationCompletionErrorText
            color: Theme.dangerText
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            visible: root.controller.locationCompletionTruncated
            text: qsTr("Showing the first 4,096 photos in this range. Narrow the capture range to review more.")
            color: Theme.textSecondary
            wrapMode: Text.WordWrap
        }

        ListView {
            id: eventList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 9
            model: root.controller.locationCompletionGroups

            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: eventCard
                required property var modelData
                width: eventList.width
                height: detailColumn.implicitHeight + 28
                radius: 8
                color: Theme.panelRaised
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    id: detailColumn
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            text: root.formatMoment(eventCard.modelData.startedAt)
                                + (Number(eventCard.modelData.endedAt)
                                    !== Number(eventCard.modelData.startedAt)
                                    ? " — " + root.formatMoment(eventCard.modelData.endedAt) : "")
                            color: Theme.textPrimary
                            font.weight: Font.DemiBold
                        }

                        Label {
                            text: qsTr("%L1 photos without location").arg(
                                Number(eventCard.modelData.targetCount))
                            color: Theme.textSecondary
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: Boolean(eventCard.modelData.hasSuggestion)
                        text: eventCard.modelData.placeName.length > 0
                            ? qsTr("Suggested from %L1 nearby located photos: %2").arg(
                                Number(eventCard.modelData.anchorCount)).arg(
                                    eventCard.modelData.placeName)
                            : qsTr("Suggested from %L1 nearby located photos").arg(
                                Number(eventCard.modelData.anchorCount))
                        color: Theme.accent
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: !Boolean(eventCard.modelData.hasSuggestion)
                        text: qsTr("No safe location suggestion for this event. Choose a place on the map.")
                        color: Theme.textMuted
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            text: eventCard.modelData.targets.length > 0
                                ? String(eventCard.modelData.targets[0].title || "") : ""
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                            elide: Text.ElideRight
                        }

                        Button {
                            text: Boolean(eventCard.modelData.hasSuggestion)
                                ? qsTr("Review suggestion") : qsTr("Choose location")
                            onClicked: root.useGroupRequested(
                                eventCard.modelData.targets,
                                Boolean(eventCard.modelData.hasSuggestion),
                                Number(eventCard.modelData.latitude),
                                Number(eventCard.modelData.longitude),
                                String(eventCard.modelData.placeName || ""))
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: !root.controller.locationCompletionBusy
                    && root.controller.locationCompletionGroups.length === 0
                    && root.controller.locationCompletionErrorText.length === 0
                text: qsTr("No photos without location in this range")
                color: Theme.textMuted
            }
        }
    }
}
