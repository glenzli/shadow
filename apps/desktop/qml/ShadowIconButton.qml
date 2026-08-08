pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Button {
    id: control

    enum Variant {
        Primary,
        Secondary,
        Ghost,
        // Quiet is kept as a named compatibility/style alias for low-emphasis
        // toolbar actions. It deliberately renders like Ghost rather than
        // silently coercing an undefined enum value to Primary.
        Quiet,
        Tinted,
        Danger
    }

    property url source
    property string toolTipText: ""
    property string accessibleName: toolTipText
    property bool selected: checkable && checked
    property int variant: ShadowIconButton.Ghost
    property int iconSize: 18
    property int buttonSize: Theme.compactControlHeight
    property int cornerRadius: Theme.compactControlRadius
    property color foregroundColor: Theme.transparent
    property color selectedSurfaceColor: Theme.accentSurfaceQuiet
    property color selectedHoverSurfaceColor: Theme.accentSurface
    property color selectedPressedSurfaceColor: Theme.accentSurfacePressed
    property color selectedOutlineColor: Theme.transparent
    property color selectedIconColor: Theme.accentSelectionText
    readonly property bool isGhostStyle: variant === ShadowIconButton.Ghost
        || variant === ShadowIconButton.Quiet

    readonly property color resolvedSurfaceColor: {
        if (!enabled)
            // Toolbar actions communicate their unavailable state through the
            // muted glyph itself. A disabled Ghost action must not become a
            // button-shaped grey tile, which reads as a different control.
            return isGhostStyle ? Theme.transparent : Theme.buttonDisabledSurface
        if (selected)
            return down ? selectedPressedSurfaceColor
                : hovered ? selectedHoverSurfaceColor : selectedSurfaceColor
        if (down) {
            if (variant === ShadowIconButton.Primary)
                return Theme.primaryActionPressed
            if (variant === ShadowIconButton.Danger)
                return Theme.dangerPressedSurface
            return isGhostStyle
                ? Theme.buttonGhostPressed : Theme.buttonPressedSurface
        }
        if (hovered) {
            if (variant === ShadowIconButton.Primary)
                return Theme.primaryActionHover
            if (variant === ShadowIconButton.Danger)
                return Theme.dangerHoverSurface
            return isGhostStyle
                ? Theme.buttonGhostHover : Theme.buttonHoverSurface
        }
        if (variant === ShadowIconButton.Primary)
            return Theme.primaryAction
        if (variant === ShadowIconButton.Tinted)
            return Theme.accentSurfaceQuiet
        if (variant === ShadowIconButton.Danger)
            return Theme.dangerSurface
        if (variant === ShadowIconButton.Secondary)
            return Theme.buttonSurface
        return Theme.transparent
    }
    readonly property color resolvedOutlineColor: {
        if (!enabled)
            return Theme.transparent
        if (selected)
            return selectedOutlineColor
        if (variant === ShadowIconButton.Primary
                || isGhostStyle
                || variant === ShadowIconButton.Tinted)
            return Theme.transparent
        return variant === ShadowIconButton.Danger
            ? Theme.dangerBorder : Theme.buttonBorder
    }
    readonly property color resolvedIconColor: {
        if (!enabled)
            return Theme.textDisabled
        if (selected)
            return selectedIconColor
        if (foregroundColor.a > 0)
            return foregroundColor
        if (variant === ShadowIconButton.Primary)
            return Theme.primaryActionForeground
        if (variant === ShadowIconButton.Tinted)
            return Theme.accentSelectionText
        if (variant === ShadowIconButton.Danger)
            return Theme.dangerText
        return hovered || visualFocus ? Theme.textPrimary : Theme.textSecondary
    }

    implicitWidth: buttonSize
    implicitHeight: buttonSize
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0
    // Disabled commands still explain why they are unavailable. Hover never
    // changes their disabled surface, but it lets the contextual tooltip be
    // read instead of turning a muted icon into a dead end.
    hoverEnabled: true
    focusPolicy: enabled ? Qt.StrongFocus : Qt.NoFocus
    Accessible.name: accessibleName
    Accessible.description: toolTipText === accessibleName ? "" : toolTipText

    background: Item {
        implicitWidth: control.buttonSize
        implicitHeight: control.buttonSize
        scale: control.down ? 0.96 : 1.0

        Rectangle {
            anchors.fill: parent
            radius: control.cornerRadius
            color: control.resolvedSurfaceColor
            border.width: 1
            border.color: control.resolvedOutlineColor
        }

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
            NumberAnimation { duration: 70 }
        }
    }

    contentItem: Item {
        implicitWidth: control.iconSize
        implicitHeight: control.iconSize
        // Color alone is too subtle for a disabled icon in a dense toolbar.
        // Reduce the glyph's opacity as well, while the Ghost surface stays
        // transparent, so unavailable commands read as inactive at a glance.
        opacity: control.enabled ? 1.0 : 0.36

        ShadowIcon {
            anchors.centerIn: parent
            source: control.source
            color: control.resolvedIconColor
            size: control.iconSize
        }
    }

    ToolTip {
        id: toolTip

        parent: control
        visible: control.hovered && !control.down
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
