import QtQuick
import QtQuick.Controls

Menu {
    popupType: Popup.Item
    padding: 4
    font.pixelSize: Theme.fontBody
    palette.text: Theme.textPrimary
    palette.windowText: Theme.textPrimary
    palette.highlight: Theme.accentSelectionSurface
    palette.highlightedText: Theme.accentSelectionText
    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.menuSurface
        border.width: 1
        border.color: Theme.borderStrong
    }
}
