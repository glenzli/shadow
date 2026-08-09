pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the advanced Library predicate editor. Every control mutates one typed
// ReviewController condition; the controller remains the sole filter authority.
Popup {
    id: root

    required property var controller

    parent: Overlay.overlay
    modal: false
    focus: true
    width: Math.min(476, parent.width - 32)
    implicitHeight: contentColumn.implicitHeight + topPadding + bottomPadding
    height: Math.min(implicitHeight, parent.height - 72)
    x: 16
    y: Math.max(16, parent.height - height - 46)
    padding: 12
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
                "text": qsTr("%1 · %L2 photos")
                    .arg(String(values[index].label))
                    .arg(Number(values[index].photoCount)),
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
    readonly property var chineseLunarMonthOptions: [
        { "text": qsTr("Any lunar month"), "value": 0 },
        { "text": qsTr("1st lunar month"), "value": 1 },
        { "text": qsTr("2nd lunar month"), "value": 2 },
        { "text": qsTr("3rd lunar month"), "value": 3 },
        { "text": qsTr("4th lunar month"), "value": 4 },
        { "text": qsTr("5th lunar month"), "value": 5 },
        { "text": qsTr("6th lunar month"), "value": 6 },
        { "text": qsTr("7th lunar month"), "value": 7 },
        { "text": qsTr("8th lunar month"), "value": 8 },
        { "text": qsTr("9th lunar month"), "value": 9 },
        { "text": qsTr("10th lunar month"), "value": 10 },
        { "text": qsTr("11th lunar month"), "value": 11 },
        { "text": qsTr("12th lunar month"), "value": 12 }
    ]
    readonly property var chineseLunarDayOptions: [
        { "text": qsTr("Any lunar day"), "value": 0 },
        { "text": qsTr("Lunar day 1"), "value": 1 },
        { "text": qsTr("Lunar day 2"), "value": 2 },
        { "text": qsTr("Lunar day 3"), "value": 3 },
        { "text": qsTr("Lunar day 4"), "value": 4 },
        { "text": qsTr("Lunar day 5"), "value": 5 },
        { "text": qsTr("Lunar day 6"), "value": 6 },
        { "text": qsTr("Lunar day 7"), "value": 7 },
        { "text": qsTr("Lunar day 8"), "value": 8 },
        { "text": qsTr("Lunar day 9"), "value": 9 },
        { "text": qsTr("Lunar day 10"), "value": 10 },
        { "text": qsTr("Lunar day 11"), "value": 11 },
        { "text": qsTr("Lunar day 12"), "value": 12 },
        { "text": qsTr("Lunar day 13"), "value": 13 },
        { "text": qsTr("Lunar day 14"), "value": 14 },
        { "text": qsTr("Lunar day 15"), "value": 15 },
        { "text": qsTr("Lunar day 16"), "value": 16 },
        { "text": qsTr("Lunar day 17"), "value": 17 },
        { "text": qsTr("Lunar day 18"), "value": 18 },
        { "text": qsTr("Lunar day 19"), "value": 19 },
        { "text": qsTr("Lunar day 20"), "value": 20 },
        { "text": qsTr("Lunar day 21"), "value": 21 },
        { "text": qsTr("Lunar day 22"), "value": 22 },
        { "text": qsTr("Lunar day 23"), "value": 23 },
        { "text": qsTr("Lunar day 24"), "value": 24 },
        { "text": qsTr("Lunar day 25"), "value": 25 },
        { "text": qsTr("Lunar day 26"), "value": 26 },
        { "text": qsTr("Lunar day 27"), "value": 27 },
        { "text": qsTr("Lunar day 28"), "value": 28 },
        { "text": qsTr("Lunar day 29"), "value": 29 },
        { "text": qsTr("Lunar day 30"), "value": 30 }
    ]
    readonly property var chineseLunarMonthTypeOptions: [
        { "text": qsTr("Any month type"), "value": "all" },
        { "text": qsTr("Regular months"), "value": "regular" },
        { "text": qsTr("Leap months"), "value": "leap" }
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
        chineseLunarMonth.currentIndex = optionIndex(
            chineseLunarMonthOptions, controller.filterChineseLunarMonth)
        chineseLunarDay.currentIndex = optionIndex(
            chineseLunarDayOptions, controller.filterChineseLunarDay)
        chineseLunarMonthType.currentIndex = optionIndex(
            chineseLunarMonthTypeOptions,
            controller.filterChineseLunarMonthType)
        const cameras = facetOptions(controller.libraryCameraFacets)
        camera.model = cameras
        camera.currentIndex = optionIndex(
            cameras, controller.filterCameraKey)
        const lenses = facetOptions(controller.libraryLensFacets)
        lens.model = lenses
        lens.currentIndex = optionIndex(
            lenses, controller.filterLensKey)
        const countries = facetOptions(controller.libraryCountryFacets)
        country.model = countries
        country.currentIndex = optionIndex(
            countries, controller.filterCountryKey)
        const cities = facetOptions(controller.libraryCityFacets)
        city.model = cities
        city.currentIndex = optionIndex(
            cities, controller.filterLocalityKey)
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
        radius: Theme.controlRadius + 1
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    component SectionLabel: Label {
        color: Theme.textMuted
        font.pixelSize: 9
        font.weight: Font.DemiBold
        font.letterSpacing: 0.7
    }

    component FilterCombo: ComboBox {
        id: combo

        implicitHeight: Theme.compactControlHeight
        leftPadding: 10
        rightPadding: 28
        topPadding: 0
        bottomPadding: 0
        hoverEnabled: true

        background: Rectangle {
            radius: Theme.compactControlRadius
            color: combo.down ? Theme.controlPressed
                : combo.hovered ? Theme.buttonHoverSurface : Theme.control
            border.width: 1
            border.color: combo.visualFocus
                ? Theme.focusRing : Theme.borderStrong
        }

        contentItem: Label {
            text: combo.displayText
            color: combo.enabled ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: Theme.fontSection
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        indicator: ShadowIcon {
            x: combo.width - width - 9
            anchors.verticalCenter: parent.verticalCenter
            size: 12
            source: "qrc:/icons/chevron-down.svg"
            color: combo.enabled ? Theme.textMuted : Theme.textDisabled
            rotation: combo.popup.visible ? 180 : 0
        }

        delegate: ItemDelegate {
            id: option

            required property int index

            width: ListView.view ? ListView.view.width : combo.width - 8
            height: 30
            leftPadding: 9
            rightPadding: 9
            highlighted: combo.highlightedIndex === index
            text: combo.textAt(index)

            background: Rectangle {
                radius: Theme.compactControlRadius
                color: option.down ? Theme.buttonGhostPressed
                    : option.highlighted || option.hovered
                        ? Theme.buttonGhostHover : Theme.transparent
            }

            contentItem: Label {
                text: option.text
                color: option.enabled ? Theme.textPrimary : Theme.textDisabled
                font.pixelSize: Theme.fontSection
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }

        popup: Popup {
            y: combo.height + 4
            width: combo.width
            implicitHeight: Math.min(combo.count * 30 + 8, 218)
            padding: 4
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.delegateModel
                currentIndex: combo.highlightedIndex
                highlightMoveDuration: 0
                ScrollIndicator.vertical: ScrollIndicator {}
            }

            background: Rectangle {
                radius: Theme.controlRadius
                color: Theme.menuSurface
                border.width: 1
                border.color: Theme.borderStrong
            }
        }
    }

    contentItem: ColumnLayout {
        id: contentColumn
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

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

        LibraryPlaceResolutionStatus {
            controller: root.controller
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: reviewGrid.implicitHeight + 16
            radius: Theme.controlRadius
            color: Theme.surfaceSubtle
            border.width: 1
            border.color: Theme.border

            GridLayout {
                id: reviewGrid
                anchors.fill: parent
                anchors.margins: 8
                columns: 3
                columnSpacing: 7
                rowSpacing: 6

                SectionLabel {
                    Layout.columnSpan: 3
                    text: qsTr("REVIEW STATE")
                }

                Item { Layout.preferredWidth: 60 }

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

                SectionLabel { text: qsTr("FLAGS") }

                FilterCombo {
                    id: includeFlag
                    Layout.fillWidth: true
                    model: root.flagOptions
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Flag must be")
                    onActivated: root.controller.filterFlag = currentValue
                }

                FilterCombo {
                    id: excludeFlag
                    Layout.fillWidth: true
                    model: root.flagOptions
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Flag must not be")
                    onActivated:
                        root.controller.filterExcludedFlag = currentValue
                }

                SectionLabel { text: qsTr("COLORS") }

                FilterCombo {
                    id: includeColor
                    Layout.fillWidth: true
                    model: root.colorOptions
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Color label must be")
                    onActivated:
                        root.controller.filterColorLabel = currentValue
                }

                FilterCombo {
                    id: excludeColor
                    Layout.fillWidth: true
                    model: root.colorOptions
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Color label must not be")
                    onActivated:
                        root.controller.filterExcludedColorLabel = currentValue
                }

                SectionLabel { text: qsTr("RATING") }

                FilterCombo {
                    id: minimumRating
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    model: root.ratingOptions
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Minimum rating")
                    onActivated:
                        root.controller.filterMinimumRating = currentValue
                }

                SectionLabel { text: qsTr("LIKE") }

                FilterCombo {
                    id: likedState
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    model: root.likedOptions
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Like state")
                    onActivated: root.controller.filterLiked = currentValue
                }

                SectionLabel { text: qsTr("EDIT") }

                FilterCombo {
                    id: editState
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    model: root.editOptions
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Edit state")
                    onActivated:
                        root.controller.filterEditState = currentValue
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: metadataGrid.implicitHeight + 16
            radius: Theme.controlRadius
            color: Theme.surfaceSubtle
            border.width: 1
            border.color: Theme.border

            GridLayout {
                id: metadataGrid
                anchors.fill: parent
                anchors.margins: 8
                columns: 2
                columnSpacing: 7
                rowSpacing: 6

                SectionLabel {
                    Layout.columnSpan: 2
                    text: qsTr("METADATA")
                }

                SectionLabel { text: qsTr("GREGORIAN DATE") }

                FilterCombo {
                    id: captureMonth
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Capture month")
                    onActivated:
                        root.controller.filterCaptureMonth = currentValue
                }

                SectionLabel { text: qsTr("CHINESE LUNAR DATE") }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    FilterCombo {
                        id: chineseLunarMonth
                        Layout.fillWidth: true
                        model: root.chineseLunarMonthOptions
                        textRole: "text"
                        valueRole: "value"
                        Accessible.name: qsTr("Chinese lunar month")
                        onActivated:
                            root.controller.filterChineseLunarMonth = currentValue
                    }

                    FilterCombo {
                        id: chineseLunarDay
                        Layout.fillWidth: true
                        model: root.chineseLunarDayOptions
                        textRole: "text"
                        valueRole: "value"
                        Accessible.name: qsTr("Chinese lunar day")
                        onActivated:
                            root.controller.filterChineseLunarDay = currentValue
                    }
                }

                SectionLabel { text: qsTr("LUNAR MONTH TYPE") }

                FilterCombo {
                    id: chineseLunarMonthType
                    Layout.fillWidth: true
                    model: root.chineseLunarMonthTypeOptions
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Chinese lunar month type")
                    onActivated:
                        root.controller.filterChineseLunarMonthType = currentValue
                }

                SectionLabel { text: qsTr("CAMERA") }

                FilterCombo {
                    id: camera
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Camera")
                    onActivated:
                        root.controller.filterCameraKey = currentValue
                }

                SectionLabel { text: qsTr("LENS") }

                FilterCombo {
                    id: lens
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Lens")
                    onActivated:
                        root.controller.filterLensKey = currentValue
                }

                SectionLabel { text: qsTr("COUNTRY") }

                FilterCombo {
                    id: country
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Country")
                    onActivated:
                        root.controller.filterCountryKey = currentValue
                }

                SectionLabel { text: qsTr("CITY") }

                FilterCombo {
                    id: city
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("City")
                    onActivated:
                        root.controller.filterLocalityKey = currentValue
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

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
