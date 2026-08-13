pragma ComponentBehavior: Bound
pragma Translator: ReviewWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// A Gallery-native, read-only review of capture-time events that do not yet
// have coordinates. The coordinator owns grouping and suggestions; this
// surface only makes each event legible before the existing preview/apply map
// flow is opened.
Rectangle {
    id: root
    objectName: "locationCompletionGallery"

    required property var workspace
    property string startDay: ""
    property string endDay: ""
    property bool active: false

    signal closeRequested()

    color: Theme.window

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
        root.workspace.controller.requestLocationCompletion(start, end)
        return true
    }

    function present() {
        root.active = true
        root.workspace.controller.refreshLocationReferenceLibraries()
        return refresh()
    }

    function dismiss() {
        root.active = false
        root.closeRequested()
    }

    function sourceForTarget(target) {
        return root.workspace.controller.locationCompletionVisualSource(
                    String(target.visualHandle || ""))
    }

    function targetsForEvent(event) {
        const targets = []
        const values = event.targets || []
        for (let index = 0; index < values.length; ++index) {
            const target = values[index]
            targets.push({
                photoId: String(target.photoId || ""),
                representationId: String(target.representationId || ""),
                sourcePath: String(target.sourcePath || ""),
                title: String(target.title || ""),
                sourceAvailable: Boolean(target.sourceAvailable),
                visualHandle: String(target.visualHandle || ""),
                visualWidth: Number(target.visualWidth || 0),
                visualHeight: Number(target.visualHeight || 0),
                visualSource: sourceForTarget(target)
            })
        }
        return targets
    }

    function openEvent(event) {
        const targets = targetsForEvent(event)
        if (targets.length === 0)
            return false
        return root.workspace.openLocationBatchForTargets(
                    targets, Boolean(event.hasSuggestion),
                    Number(event.latitude), Number(event.longitude),
                    String(event.placeName || ""),
                    Boolean(event.hasSuggestion)
                        ? "location-completion:event-anchor" : "manual-map")
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.active && !root.workspace.controller.libraryMetadataBusy
        onActivated: root.dismiss()
    }

    FolderDialog {
        id: referenceFolderDialog
        title: qsTr("Choose a reference photo or video folder")
        onAccepted: root.workspace.controller.addLocationReferenceLibrary(selectedFolder, 0)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: header.implicitHeight + 20
            color: Theme.chrome

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.border
            }

            RowLayout {
                id: header
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 16
                spacing: 10

                ShadowIconButton {
                    source: "qrc:/icons/back-to-library.svg"
                    toolTipText: qsTr("Return to Library")
                    accessibleName: toolTipText
                    onClicked: root.dismiss()
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    Label {
                        text: qsTr("Complete locations")
                        color: Theme.textPrimary
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }

                    Label {
                        text: qsTr("Review capture events, then preview a location before applying it.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontMeta
                    }
                }

                Label {
                    visible: root.workspace.controller.locationCompletionBusy
                    text: qsTr("Finding events…")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }

                Button {
                    text: qsTr("Reference folders")
                    enabled: !root.workspace.controller.locationReferenceBusy
                    onClicked: referencePopup.open()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 18
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
                    enabled: !root.workspace.controller.locationCompletionBusy
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
                    enabled: !root.workspace.controller.locationCompletionBusy
                    onEditingFinished: root.endDay = text.trim()
                }

                Button {
                    objectName: "locationCompletionRefreshButton"
                    text: qsTr("Refresh")
                    enabled: !root.workspace.controller.locationCompletionBusy
                    onClicked: {
                        root.startDay = startField.text.trim()
                        root.endDay = endField.text.trim()
                        root.refresh()
                    }
                }

                Item { Layout.fillWidth: true }

                Label {
                    visible: root.startDay.trim().length > 0 || root.endDay.trim().length > 0
                    text: qsTr("Only photos inside this capture range are considered.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.topMargin: 10
            visible: root.workspace.controller.locationCompletionErrorText.length > 0
            text: root.workspace.controller.locationCompletionErrorText
            color: Theme.dangerText
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.topMargin: 10
            visible: root.workspace.controller.locationCompletionTruncated
            text: qsTr("Showing the first 4,096 photos in this range. Narrow the capture range to review more.")
            color: Theme.textSecondary
            wrapMode: Text.WordWrap
        }

        ListView {
            id: eventList
            objectName: "locationCompletionEventGallery"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.topMargin: 14
            Layout.bottomMargin: 18
            clip: true
            spacing: 18
            model: root.workspace.controller.locationCompletionGroups

            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: eventCard
                required property var modelData
                width: eventList.width
                height: eventContent.implicitHeight + 28
                radius: 10
                color: Theme.panelRaised
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    id: eventContent
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                text: root.formatMoment(eventCard.modelData.startedAt)
                                    + (Number(eventCard.modelData.endedAt)
                                        !== Number(eventCard.modelData.startedAt)
                                        ? " — " + root.formatMoment(eventCard.modelData.endedAt) : "")
                                color: Theme.textPrimary
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                            }

                            Label {
                                text: Boolean(eventCard.modelData.hasSuggestion)
                                    ? (String(eventCard.modelData.placeName || "").length > 0
                                        ? qsTr("Suggested: %1 · %2 nearby anchors").arg(
                                            String(eventCard.modelData.placeName)).arg(
                                                Number(eventCard.modelData.anchorCount))
                                        : qsTr("Suggested from %1 nearby anchors").arg(
                                            Number(eventCard.modelData.anchorCount)))
                                    : qsTr("No safe suggestion — choose a location on the map")
                                color: Boolean(eventCard.modelData.hasSuggestion)
                                    ? Theme.accent : Theme.textMuted
                                font.pixelSize: Theme.fontMeta
                                elide: Text.ElideRight
                            }
                        }

                        Label {
                            text: qsTr("%L1 photos").arg(Number(eventCard.modelData.targetCount))
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                        }

                        Button {
                            text: Boolean(eventCard.modelData.hasSuggestion)
                                ? qsTr("Preview suggestion") : qsTr("Choose location")
                            onClicked: root.openEvent(eventCard.modelData)
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 8

                        Repeater {
                            model: eventCard.modelData.targets || []

                            delegate: Rectangle {
                                required property var modelData
                                width: 150
                                height: 128
                                radius: 7
                                clip: true
                                color: Theme.panel
                                border.width: 1
                                border.color: Theme.border

                                Image {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    height: 96
                                    source: root.sourceForTarget(modelData)
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    cache: true
                                    smooth: true
                                    mipmap: true
                                    sourceSize.width: 300
                                    sourceSize.height: 300
                                }

                                Label {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: 7
                                    text: String(modelData.title || "")
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontMeta
                                    elide: Text.ElideRight
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.workspace.selectPhoto({
                                        photoId: String(modelData.photoId || ""),
                                        representationId: String(modelData.representationId || ""),
                                        sourcePath: String(modelData.sourcePath || ""),
                                        sourceAvailable: Boolean(modelData.sourceAvailable),
                                        title: String(modelData.title || ""),
                                        visualHandle: String(modelData.visualHandle || ""),
                                        visualSource: root.sourceForTarget(modelData),
                                        visualWidth: Number(modelData.visualWidth || 0),
                                        visualHeight: Number(modelData.visualHeight || 0)
                                    }, mouse.modifiers)
                                }
                            }
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: Math.min(420, parent.width - 60)
                visible: !root.workspace.controller.locationCompletionBusy
                    && root.workspace.controller.locationCompletionGroups.length === 0
                    && root.workspace.controller.locationCompletionErrorText.length === 0
                text: qsTr("No photos without location in this range")
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }

    Popup {
        id: referencePopup
        x: root.width - width - 18
        y: 42
        width: 390
        padding: 14
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        contentItem: ColumnLayout {
            spacing: 10

            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Reference photo and video folders")
                    color: Theme.textPrimary
                    font.weight: Font.DemiBold
                }

                Button {
                    objectName: "locationReferenceAddButton"
                    text: qsTr("Add folder")
                    enabled: !root.workspace.controller.locationReferenceBusy
                    onClicked: referenceFolderDialog.open()
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Read only capture time and GPS from a phone or camera folder. These files are not imported into your Library.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: root.workspace.controller.locationReferenceErrorText.length > 0
                text: root.workspace.controller.locationReferenceErrorText
                color: Theme.dangerText
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: root.workspace.controller.locationReferenceLibraries.length === 0
                    && !root.workspace.controller.locationReferenceBusy
                text: qsTr("No reference folder added")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
            }

            Repeater {
                id: referenceList
                objectName: "locationReferenceList"
                model: root.workspace.controller.locationReferenceLibraries

                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: row.implicitHeight + 12
                    radius: 6
                    color: Theme.panel

                    RowLayout {
                        id: row
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
                            enabled: !root.workspace.controller.locationReferenceBusy
                            Accessible.name: qsTr("Clock offset seconds")
                        }

                        Button {
                            text: qsTr("Rescan")
                            enabled: !root.workspace.controller.locationReferenceBusy
                            onClicked: root.workspace.controller.addLocationReferenceLibrary(
                                modelData.rootUrl, offsetBox.value)
                        }

                        Button {
                            text: qsTr("Remove")
                            enabled: !root.workspace.controller.locationReferenceBusy
                            onClicked: root.workspace.controller.removeLocationReferenceLibrary(
                                String(modelData.id || ""))
                        }
                    }
                }
            }
        }
    }
}
