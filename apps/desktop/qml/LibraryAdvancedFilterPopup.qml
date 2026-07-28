pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the advanced Library predicate editor. Every row mutates one typed
// ReviewController condition; the controller remains the sole filter authority.
Popup {
    id: root

    required property var controller

    parent: Overlay.overlay
    modal: false
    focus: true
    width: Math.min(560, parent.width - 32)
    height: Math.min(570, parent.height - 72)
    x: 16
    y: Math.max(16, parent.height - height - 46)
    padding: 14
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function optionIndex(model, value) {
        for (let index = 0; index < model.length; ++index) {
            if (String(model[index].value) === String(value))
                return index
        }
        return 0
    }

    function facetOptions(values) {
        const options = [
            { "text": qsTr("Any"), "value": "" }
        ]
        for (let index = 0; index < values.length; ++index) {
            options.push({
                "text": String(values[index].label),
                "value": String(values[index].key)
            })
        }
        return options
    }

    readonly property var flagOptions: [
        { "text": qsTr("Any"), "value": "all" },
        { "text": qsTr("Unflagged"), "value": "unflagged" },
        { "text": qsTr("Flagged"), "value": "picked" },
        { "text": qsTr("Rejected"), "value": "rejected" }
    ]
    readonly property var colorOptions: [
        { "text": qsTr("Any"), "value": "all" },
        { "text": qsTr("No color label"), "value": "none" },
        { "text": qsTr("Red"), "value": "red" },
        { "text": qsTr("Yellow"), "value": "yellow" },
        { "text": qsTr("Green"), "value": "green" },
        { "text": qsTr("Blue"), "value": "blue" },
        { "text": qsTr("Purple"), "value": "purple" }
    ]
    readonly property var ratingOptions: [
        { "text": qsTr("Any rating"), "value": 0 },
        { "text": qsTr("1 star or more"), "value": 1 },
        { "text": qsTr("2 stars or more"), "value": 2 },
        { "text": qsTr("3 stars or more"), "value": 3 },
        { "text": qsTr("4 stars or more"), "value": 4 },
        { "text": qsTr("5 stars"), "value": 5 }
    ]
    readonly property var likedOptions: [
        { "text": qsTr("Any"), "value": "all" },
        { "text": qsTr("Liked"), "value": "liked" },
        { "text": qsTr("Not liked"), "value": "unliked" }
    ]
    readonly property var editOptions: [
        { "text": qsTr("Any"), "value": "all" },
        { "text": qsTr("Edited"), "value": "edited" },
        { "text": qsTr("Not edited"), "value": "unedited" }
    ]

    function syncControls() {
        includeFlag.currentIndex = optionIndex(
            flagOptions, controller.filterFlag)
        excludeFlag.currentIndex = optionIndex(
            flagOptions, controller.filterExcludedFlag)
        includeColor.currentIndex = optionIndex(
            colorOptions, controller.filterColorLabel)
        excludeColor.currentIndex = optionIndex(
            colorOptions, controller.filterExcludedColorLabel)
        minimumRating.currentIndex = optionIndex(
            ratingOptions, controller.filterMinimumRating)
        likedState.currentIndex = optionIndex(
            likedOptions, controller.filterLiked)
        editState.currentIndex = optionIndex(
            editOptions, controller.filterEditState)

        const months = facetOptions(controller.libraryCaptureMonthFacets)
        captureMonth.model = months
        captureMonth.currentIndex = optionIndex(
            months, controller.filterCaptureMonth)
        const cameras = facetOptions(controller.libraryCameraFacets)
        camera.model = cameras
        camera.currentIndex = optionIndex(
            cameras, controller.filterCameraKey)
        const lenses = facetOptions(controller.libraryLensFacets)
        lens.model = lenses
        lens.currentIndex = optionIndex(
            lenses, controller.filterLensKey)
    }

    onOpened: {
        controller.refreshLibraryFacets()
        syncControls()
    }

    Connections {
        target: root.controller
        function onFiltersChanged() {
            root.syncControls()
        }
        function onLibraryFacetsChanged() {
            root.syncControls()
        }
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: 10

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: qsTr("Advanced Library Filters")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSection
                font.weight: Font.DemiBold
            }

            ShadowIconButton {
                source: "qrc:/icons/clear.svg"
                buttonSize: 26
                iconSize: 13
                toolTipText: qsTr("Close advanced filters")
                accessibleName: toolTipText
                onClicked: root.close()
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("All active conditions must match. “Must not be” adds an explicit exclusion.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 3
            columnSpacing: 8
            rowSpacing: 8

            Item {
                Layout.preferredWidth: 62
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Must be")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                horizontalAlignment: Text.AlignHCenter
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Must not be")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                horizontalAlignment: Text.AlignHCenter
            }

            Label {
                text: qsTr("FLAGS")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: includeFlag
                Layout.fillWidth: true
                model: root.flagOptions
                textRole: "text"
                valueRole: "value"
                onActivated: root.controller.filterFlag = currentValue
                Accessible.name: qsTr("Flag must be")
            }
            ComboBox {
                id: excludeFlag
                Layout.fillWidth: true
                model: root.flagOptions
                textRole: "text"
                valueRole: "value"
                onActivated:
                    root.controller.filterExcludedFlag = currentValue
                Accessible.name: qsTr("Flag must not be")
            }

            Label {
                text: qsTr("COLORS")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: includeColor
                Layout.fillWidth: true
                model: root.colorOptions
                textRole: "text"
                valueRole: "value"
                onActivated:
                    root.controller.filterColorLabel = currentValue
                Accessible.name: qsTr("Color label must be")
            }
            ComboBox {
                id: excludeColor
                Layout.fillWidth: true
                model: root.colorOptions
                textRole: "text"
                valueRole: "value"
                onActivated:
                    root.controller.filterExcludedColorLabel = currentValue
                Accessible.name: qsTr("Color label must not be")
            }

            Label {
                text: qsTr("RATING")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: minimumRating
                Layout.columnSpan: 2
                Layout.fillWidth: true
                model: root.ratingOptions
                textRole: "text"
                valueRole: "value"
                onActivated:
                    root.controller.filterMinimumRating = currentValue
            }

            Label {
                text: qsTr("LIKE")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: likedState
                Layout.columnSpan: 2
                Layout.fillWidth: true
                model: root.likedOptions
                textRole: "text"
                valueRole: "value"
                onActivated: root.controller.filterLiked = currentValue
            }

            Label {
                text: qsTr("EDIT")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: editState
                Layout.columnSpan: 2
                Layout.fillWidth: true
                model: root.editOptions
                textRole: "text"
                valueRole: "value"
                onActivated:
                    root.controller.filterEditState = currentValue
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 8
            rowSpacing: 8

            Label {
                text: qsTr("DATE")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: captureMonth
                Layout.fillWidth: true
                textRole: "text"
                valueRole: "value"
                onActivated:
                    root.controller.filterCaptureMonth = currentValue
            }

            Label {
                text: qsTr("CAMERA")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: camera
                Layout.fillWidth: true
                textRole: "text"
                valueRole: "value"
                onActivated:
                    root.controller.filterCameraKey = currentValue
            }

            Label {
                text: qsTr("LENS")
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: lens
                Layout.fillWidth: true
                textRole: "text"
                valueRole: "value"
                onActivated:
                    root.controller.filterLensKey = currentValue
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true

            ShadowButton {
                compact: true
                variant: ShadowButton.Ghost
                text: qsTr("Clear all")
                onClicked: root.controller.clearFilters()
            }

            Item { Layout.fillWidth: true }

            ShadowButton {
                compact: true
                text: qsTr("Done")
                onClicked: root.close()
            }
        }
    }
}
