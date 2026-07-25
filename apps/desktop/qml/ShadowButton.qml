pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Button {
    id: control

    enum Variant {
        Primary,
        Secondary,
        Ghost,
        Tinted,
        Danger
    }

    property int variant: ShadowButton.Secondary
    property bool selected: false
    property bool compact: false
    property string toolTipText: ""
    property string accessibleName: text.length > 0 ? text : toolTipText
    property int cornerRadius: compact
        ? Theme.compactControlRadius : Theme.controlRadius
    property int minimumButtonWidth: compact ? 40 : 68

    property color surfaceColor: {
        switch (variant) {
        case ShadowButton.Primary:
            return Theme.primaryAction
        case ShadowButton.Ghost:
            return Theme.transparent
        case ShadowButton.Tinted:
            return Theme.accentSurface
        case ShadowButton.Danger:
            return Theme.dangerSurface
        default:
            return Theme.buttonSurface
        }
    }
    property color hoverSurfaceColor: {
        switch (variant) {
        case ShadowButton.Primary:
            return Theme.primaryActionHover
        case ShadowButton.Ghost:
            return Theme.buttonGhostHover
        case ShadowButton.Tinted:
            return Theme.accentSurface
        case ShadowButton.Danger:
            return Theme.dangerHoverSurface
        default:
            return Theme.buttonHoverSurface
        }
    }
    property color pressedSurfaceColor: {
        switch (variant) {
        case ShadowButton.Primary:
            return Theme.primaryActionPressed
        case ShadowButton.Ghost:
            return Theme.buttonGhostPressed
        case ShadowButton.Tinted:
            return Theme.accentSurfacePressed
        case ShadowButton.Danger:
            return Theme.dangerPressedSurface
        default:
            return Theme.buttonPressedSurface
        }
    }
    property color outlineColor: {
        switch (variant) {
        case ShadowButton.Primary:
            return Theme.transparent
        case ShadowButton.Ghost:
            return Theme.transparent
        case ShadowButton.Tinted:
            return Theme.transparent
        case ShadowButton.Danger:
            return Theme.dangerBorder
        default:
            return Theme.buttonBorder
        }
    }
    property color foregroundColor: {
        switch (variant) {
        case ShadowButton.Primary:
            return Theme.primaryActionForeground
        case ShadowButton.Tinted:
            return Theme.accentSelectionText
        case ShadowButton.Danger:
            return Theme.dangerText
        default:
            return Theme.textSecondary
        }
    }
    property color selectedSurfaceColor: Theme.accentSelectionSurface
    property color selectedHoverSurfaceColor: selectedSurfaceColor
    property color selectedPressedSurfaceColor: selectedSurfaceColor
    property color selectedOutlineColor: Theme.transparent
    property color selectedForegroundColor: Theme.accentSelectionText

    readonly property color resolvedSurfaceColor: !enabled
        ? (variant === ShadowButton.Ghost
            ? Theme.buttonDisabledGhostSurface : Theme.buttonDisabledSurface)
        : selected
            ? (down ? selectedPressedSurfaceColor
                : hovered ? selectedHoverSurfaceColor
                : selectedSurfaceColor)
            : down ? pressedSurfaceColor
            : hovered ? hoverSurfaceColor
            : surfaceColor
    readonly property color resolvedOutlineColor: !enabled
        ? Theme.transparent
        : selected ? selectedOutlineColor : outlineColor
    readonly property color resolvedForegroundColor: !enabled
        ? Theme.textDisabled
        : selected ? selectedForegroundColor : foregroundColor

    implicitWidth: Math.max(
        minimumButtonWidth,
        contentItem.implicitWidth + leftPadding + rightPadding
    )
    implicitHeight: compact
        ? Theme.compactControlHeight : Theme.controlHeight
    leftPadding: compact ? 10 : 14
    rightPadding: compact ? 10 : 14
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: enabled
    focusPolicy: enabled ? Qt.StrongFocus : Qt.NoFocus
    Accessible.name: accessibleName
    Accessible.description: toolTipText.length > 0
        && toolTipText !== accessibleName ? toolTipText : ""

    background: Item {
        implicitWidth: control.minimumButtonWidth
        implicitHeight: control.compact
            ? Theme.compactControlHeight : Theme.controlHeight
        scale: control.down ? 0.985 : 1.0

        Rectangle {
            anchors.fill: parent
            radius: control.cornerRadius
            color: control.resolvedSurfaceColor
            border.width: 1
            border.color: control.resolvedOutlineColor
        }

        // Keyboard focus is an outer ring. It never replaces or thickens the
        // semantic selected/variant border, avoiding the old double inset.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            visible: control.enabled && control.visualFocus
            radius: control.cornerRadius + 2
            color: Theme.transparent
            border.width: 1
            border.color: Theme.focusRing
        }

        Behavior on scale {
            NumberAnimation { duration: 80 }
        }
    }

    contentItem: Label {
        text: control.text
        color: control.resolvedForegroundColor
        font.pixelSize: control.compact ? Theme.fontSection : Theme.fontBody
        font.weight: control.variant === ShadowButton.Primary
            ? Font.DemiBold : Font.Medium
        font.letterSpacing: control.compact ? 0.1 : 0.2
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight

        Behavior on color {
            ColorAnimation { duration: 90 }
        }
    }

    ToolTip {
        id: toolTip

        parent: control
        visible: control.enabled && control.hovered && !control.down
            && control.toolTipText.length > 0
        delay: 450
        timeout: 4000
        text: control.toolTipText
        x: Math.round((control.width - width) / 2)
        y: control.height + 6
        margins: 6
        topPadding: 6
        bottomPadding: 6
        leftPadding: 9
        rightPadding: 9

        contentItem: Label {
            text: toolTip.text
            color: Theme.textPrimary
            font.pixelSize: Theme.fontMeta
            font.weight: Font.Medium
        }

        background: Rectangle {
            radius: Theme.compactControlRadius
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
    }
}
