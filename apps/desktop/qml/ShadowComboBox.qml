pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

ComboBox {
    id: control

    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(120, implicitContentWidth + leftPadding + rightPadding)
    font.pixelSize: Theme.fontBody
    leftPadding: 10
    rightPadding: 30
    hoverEnabled: true

    contentItem: Text {
        text: control.displayText
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: ShadowIcon {
        x: control.width - width - 9
        y: (control.height - height) / 2
        width: 14
        height: 14
        source: "qrc:/icons/chevron-down.svg"
        color: control.enabled ? Theme.textSecondary : Theme.textDisabled
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: !control.enabled ? Theme.controlDisabled
            : control.down ? Theme.buttonPressedSurface
            : control.hovered ? Theme.buttonHoverSurface : Theme.control
        border.width: 1
        border.color: control.activeFocus ? Theme.focusRing
            : control.enabled ? Theme.borderStrong : Theme.borderDisabled
    }

    delegate: ItemDelegate {
        id: option
        required property int index
        width: control.popup.availableWidth
        implicitHeight: Theme.controlHeight
        text: control.textAt(index)
        font: control.font
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: option.text
            font: control.font
            color: option.highlighted ? Theme.accentSelectionText : Theme.textPrimary
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: Theme.compactControlRadius
            color: option.highlighted ? Theme.accentSelectionSurface : Theme.transparent
        }
    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        padding: 4
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, 280)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { }
        }
        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.menuSurface
            border.width: 1
            border.color: Theme.borderStrong
        }
    }
}
