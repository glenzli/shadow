pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the gallery grouping trigger, anchored menu, and the generic dimension
// registry. The C++ compiler supplies dimension descriptors, so future people
// or structured-place providers do not require a new popup interaction model.
Item {
    id: control

    required property var grouping

    implicitWidth: Theme.compactControlHeight
    implicitHeight: Theme.compactControlHeight

    function dimensionsInCategory(category) {
        const result = []
        const dimensions = control.grouping.dimensions
        for (let index = 0; index < dimensions.length; ++index) {
            if (String(dimensions[index].category) === category)
                result.push(dimensions[index])
        }
        return result
    }

    function dimensionsOutsideCategories(categories) {
        const result = []
        const dimensions = control.grouping.dimensions
        for (let index = 0; index < dimensions.length; ++index) {
            if (categories.indexOf(String(dimensions[index].category)) < 0)
                result.push(dimensions[index])
        }
        return result
    }

    readonly property var dateDimensions: dimensionsInCategory("date")
    readonly property var placeDimensions: dimensionsInCategory("place")
    readonly property var fallbackDimensions:
        dimensionsOutsideCategories(["date", "place"])

    component SegmentOption: Button {
        id: option

        required property var descriptor

        implicitHeight: 32
        leftPadding: 6
        rightPadding: 6
        topPadding: 0
        bottomPadding: 0
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: String(descriptor.label)
        onClicked: control.grouping.setDimensionSelected(
            String(descriptor.key), !Boolean(descriptor.selected))

        background: Rectangle {
            radius: 6
            color: option.down ? Theme.buttonGhostPressed
                : option.hovered || option.visualFocus
                    ? Theme.buttonGhostHover
                    : Boolean(option.descriptor.selected)
                        ? Theme.accentSurfaceQuiet : Theme.transparent
        }

        contentItem: Label {
            text: String(option.descriptor.label)
            color: Boolean(option.descriptor.selected)
                ? Theme.accentTextMuted : Theme.textSecondary
            font.pixelSize: Theme.fontBody
            font.weight: Boolean(option.descriptor.selected)
                ? Font.DemiBold : Font.Medium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    component ToggleOption: Button {
        id: toggleOption

        required property var descriptor
        property url iconSource: "qrc:/icons/source-stack.svg"

        implicitHeight: 40
        leftPadding: 8
        rightPadding: 8
        topPadding: 0
        bottomPadding: 0
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: String(descriptor.label)
        onClicked: control.grouping.setDimensionSelected(
            String(descriptor.key), !Boolean(descriptor.selected))

        background: Rectangle {
            radius: 7
            color: toggleOption.down ? Theme.buttonGhostPressed
                : toggleOption.hovered || toggleOption.visualFocus
                    ? Theme.buttonGhostHover
                    : Boolean(toggleOption.descriptor.selected)
                        ? Theme.accentSurfaceQuiet : Theme.surfaceSubtle
        }

        contentItem: RowLayout {
            spacing: 8

            Rectangle {
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                radius: 6
                color: Boolean(toggleOption.descriptor.selected)
                    ? Theme.accentSurface : Theme.panelRaised

                ShadowIcon {
                    anchors.centerIn: parent
                    source: toggleOption.iconSource
                    color: Boolean(toggleOption.descriptor.selected)
                        ? Theme.accentTextMuted : Theme.textSecondary
                    size: 14
                }
            }

            Label {
                Layout.fillWidth: true
                text: String(toggleOption.descriptor.label)
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
            }

            Rectangle {
                Layout.preferredWidth: 28
                Layout.preferredHeight: 16
                radius: height / 2
                color: Boolean(toggleOption.descriptor.selected)
                    ? Theme.switchOnSurface : Theme.switchOffSurface

                Rectangle {
                    x: Boolean(toggleOption.descriptor.selected)
                        ? parent.width - width - 3 : 3
                    anchors.verticalCenter: parent.verticalCenter
                    width: 10
                    height: 10
                    radius: width / 2
                    color: Boolean(toggleOption.descriptor.selected)
                        ? Theme.accent : Theme.textMuted

                    Behavior on x {
                        NumberAnimation { duration: 110 }
                    }
                }
            }
        }
    }

    ShadowIconButton {
        id: groupButton
        objectName: "reviewGalleryGroupingButton"
        anchors.centerIn: parent
        source: "qrc:/icons/source-stack.svg"
        selected: groupingPopup.visible
            || control.grouping.activeDimensionCount > 0
        variant: ShadowIconButton.Ghost
        toolTipText: control.grouping.activeDimensionCount > 0
            ? qsTr("Grouped by %1").arg(control.grouping.activeSummary)
            : qsTr("Group photos")
        accessibleName: toolTipText
        onClicked: groupingPopup.visible
            ? groupingPopup.close() : groupingPopup.open()
    }

    Popup {
        id: groupingPopup
        objectName: "reviewGalleryGroupingPopup"
        // Popup maps its visual parent into the overlay. Keeping the trigger as
        // the coordinate owner avoids full-window offsets on framed macOS windows.
        parent: control
        x: control.width - width
        y: control.height + 6
        width: 286
        height: groupingColumn.implicitHeight + topPadding + bottomPadding
        padding: 10
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 10
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }

        contentItem: ColumnLayout {
            id: groupingColumn
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    radius: 7
                    color: Theme.surfaceSubtle

                    ShadowIcon {
                        anchors.centerIn: parent
                        source: "qrc:/icons/source-stack.svg"
                        color: Theme.textSecondary
                        size: 15
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("GROUP PHOTOS")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody + 1
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.3
                }

                Label {
                    visible: control.grouping.activeDimensionCount > 0
                    text: control.grouping.activeSummary
                    color: Theme.accentTextQuiet
                    font.pixelSize: Theme.fontSection
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    Layout.maximumWidth: 112
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.border
            }

            ColumnLayout {
                visible: control.dateDimensions.length > 0
                Layout.fillWidth: true
                spacing: 4

                Label {
                    text: control.dateDimensions.length > 0
                        ? String(control.dateDimensions[0].categoryLabel) : ""
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSection
                    font.weight: Font.DemiBold
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    radius: 7
                    color: Theme.panelInset

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 2
                        spacing: 2

                        Repeater {
                            model: control.dateDimensions

                            SegmentOption {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                descriptor: modelData
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                visible: control.placeDimensions.length > 0
                Layout.fillWidth: true
                spacing: 4

                Label {
                    text: control.placeDimensions.length > 0
                        ? String(control.placeDimensions[0].categoryLabel) : ""
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSection
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: control.placeDimensions

                    ToggleOption {
                        required property var modelData
                        Layout.fillWidth: true
                        descriptor: modelData
                        iconSource: "qrc:/icons/location-pin.svg"
                    }
                }
            }

            Repeater {
                model: control.fallbackDimensions

                delegate: ColumnLayout {
                    id: dimensionRow
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 4

                    Rectangle {
                        visible: dimensionRow.modelData.firstInCategory
                        Layout.fillWidth: true
                        height: visible ? 1 : 0
                        color: Theme.border
                    }

                    Label {
                        visible: dimensionRow.modelData.firstInCategory
                        Layout.fillWidth: true
                        text: String(dimensionRow.modelData.categoryLabel)
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSection
                        font.weight: Font.DemiBold
                    }

                    ToggleOption {
                        Layout.fillWidth: true
                        descriptor: dimensionRow.modelData
                    }
                }
            }

            Rectangle {
                visible: control.grouping.activeDimensionCount > 0
                Layout.fillWidth: true
                height: visible ? 1 : 0
                color: Theme.border
            }

            ShadowButton {
                visible: control.grouping.activeDimensionCount > 0
                Layout.alignment: Qt.AlignRight
                text: qsTr("Clear grouping")
                variant: ShadowButton.Ghost
                minimumButtonWidth: 0
                onClicked: control.grouping.clearGrouping()
            }
        }
    }
}
