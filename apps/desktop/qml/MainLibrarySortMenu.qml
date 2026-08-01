pragma ComponentBehavior: Bound
pragma Translator: "Main"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the Library sort choices and their anchored Shadow-styled popup.
// Keeping placement with the popup avoids platform-native menu geometry and
// makes the bottom status-bar trigger behave consistently on every desktop.
Popup {
    id: root

    required property var controller

    parent: Overlay.overlay
    width: 244
    padding: 6
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function presentFrom(anchorItem) {
        parent = Overlay.overlay
        const point = anchorItem.mapToItem(Overlay.overlay, 0, 0)
        x = Math.max(8, Math.min(
            point.x, Overlay.overlay.width - width - 8))
        y = Math.max(8, point.y - implicitHeight - 6)
        open()
    }

    function choose(key, descending) {
        controller.librarySortKey = key
        controller.librarySortDescending = descending
        close()
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.menuSurface
        border.width: 1
        border.color: Theme.borderStrong
    }

    component Divider: Rectangle {
        width: parent ? parent.width : 0
        height: 1
        color: Theme.border
    }

    component SortOption: Button {
        id: option

        required property string optionText
        required property bool active
        signal chosen()

        width: parent ? parent.width : 0
        height: 34
        leftPadding: 9
        rightPadding: 9
        topPadding: 0
        bottomPadding: 0
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: optionText
        onClicked: chosen()

        background: Rectangle {
            radius: Theme.compactControlRadius
            color: option.down ? Theme.buttonGhostPressed
                : option.hovered || option.visualFocus
                    ? Theme.buttonGhostHover
                    : option.active ? Theme.accentSurfaceQuiet
                    : Theme.transparent
        }

        contentItem: RowLayout {
            spacing: 8

            ShadowIcon {
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                source: "qrc:/icons/check.svg"
                color: Theme.accent
                opacity: option.active ? 1 : 0
            }

            Label {
                Layout.fillWidth: true
                text: option.optionText
                color: option.active ? Theme.textPrimary : Theme.textSecondary
                font.pixelSize: Theme.fontSection
                font.weight: option.active ? Font.DemiBold : Font.Medium
                elide: Text.ElideRight
            }
        }
    }

    contentItem: Column {
        width: root.width - root.leftPadding - root.rightPadding
        spacing: 2

        SortOption {
            optionText: qsTr("Capture date · Newest first")
            active: root.controller.librarySortKey === "capture_time"
                && root.controller.librarySortDescending
            onChosen: root.choose("capture_time", true)
        }

        SortOption {
            optionText: qsTr("Capture date · Oldest first")
            active: root.controller.librarySortKey === "capture_time"
                && !root.controller.librarySortDescending
            onChosen: root.choose("capture_time", false)
        }

        Divider {}

        SortOption {
            optionText: qsTr("Name · A to Z")
            active: root.controller.librarySortKey === "name"
                && !root.controller.librarySortDescending
            onChosen: root.choose("name", false)
        }

        SortOption {
            optionText: qsTr("Name · Z to A")
            active: root.controller.librarySortKey === "name"
                && root.controller.librarySortDescending
            onChosen: root.choose("name", true)
        }
    }
}
