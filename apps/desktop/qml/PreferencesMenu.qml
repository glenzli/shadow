pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects

Menu {
    id: root

    required property var preferences
    signal openLutLibraryRequested()
    signal openCacheMaintenanceRequested()
    signal openMapProviderSettingsRequested()

    title: qsTr("Settings")
    width: 236
    topPadding: 7
    bottomPadding: 7
    leftPadding: 7
    rightPadding: 7
    margins: 8
    overlap: 0
    transformOrigin: Item.TopRight
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    ButtonGroup {
        id: appearanceGroup
    }

    ButtonGroup {
        id: languageGroup
    }

    component PreferenceSection: Item {
        required property string text

        width: root.availableWidth
        implicitHeight: 27

        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 11
            anchors.rightMargin: 11
            anchors.bottomMargin: 5
            text: parent.text
            color: Theme.textMuted
            font.pixelSize: 10
            font.weight: Font.DemiBold
            font.letterSpacing: 0.35
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
    }

    component PreferenceSeparator: Item {
        width: root.availableWidth
        implicitHeight: 13

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 11
            anchors.rightMargin: 11
            height: 1
            color: Theme.border
        }
    }

    component PreferenceItem: MenuItem {
        id: control

        width: root.availableWidth
        implicitHeight: Theme.controlHeight
        leftPadding: 11
        rightPadding: 11
        topPadding: 0
        bottomPadding: 0
        spacing: 9
        checkable: true

        contentItem: Label {
            leftPadding: 24
            text: control.text
            color: control.enabled
                ? (control.checked ? Theme.accent : Theme.textPrimary)
                : Theme.textDisabled
            font.pixelSize: 12
            font.weight: control.checked ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignVCenter

            Behavior on color {
                ColorAnimation { duration: 80 }
            }
        }

        indicator: Item {
            x: control.leftPadding + 3
            y: Math.round((control.height - height) / 2)
            implicitWidth: 14
            implicitHeight: 14

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Theme.transparent
                border.width: 1
                border.color: control.checked
                    ? Theme.accent : Theme.borderEmphasis

                Behavior on border.color {
                    ColorAnimation { duration: 80 }
                }
            }

            Rectangle {
                anchors.centerIn: parent
                width: 6
                height: 6
                radius: 3
                color: Theme.accent
                opacity: control.checked ? 1 : 0
                scale: control.checked ? 1 : 0.45

                Behavior on opacity {
                    NumberAnimation { duration: 90 }
                }

                Behavior on scale {
                    NumberAnimation {
                        duration: 110
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }

        arrow: null

        background: Rectangle {
            x: 2
            y: 1
            width: control.width - 4
            height: control.height - 2
            radius: Theme.controlRadius
            color: {
                if (control.down)
                    return control.checked
                        ? Theme.accentSurfacePressed : Theme.buttonGhostPressed
                if (control.highlighted)
                    return control.checked
                        ? Theme.accentSurface : Theme.buttonGhostHover
                return control.checked
                    ? Theme.accentSurfaceQuiet : Theme.transparent
            }
            border.width: control.visualFocus ? 1 : 0
            border.color: Theme.focusRing
        }
    }

    PreferenceSection {
        text: qsTr("Appearance")
    }

    PreferenceItem {
        text: qsTr("Follow System")
        checked: root.preferences.appearanceMode === "system"
        ButtonGroup.group: appearanceGroup
        onTriggered: root.preferences.appearanceMode = "system"
    }

    PreferenceItem {
        text: qsTr("Light")
        checked: root.preferences.appearanceMode === "light"
        ButtonGroup.group: appearanceGroup
        onTriggered: root.preferences.appearanceMode = "light"
    }

    PreferenceItem {
        text: qsTr("Dark")
        checked: root.preferences.appearanceMode === "dark"
        ButtonGroup.group: appearanceGroup
        onTriggered: root.preferences.appearanceMode = "dark"
    }

    PreferenceSeparator {}

    PreferenceSection {
        text: qsTr("Library")
    }

    MenuItem {
        id: lutLibraryItem
        width: root.availableWidth
        height: Theme.controlHeight
        text: qsTr("LUT Library…")
        leftPadding: 35
        contentItem: Label {
            text: lutLibraryItem.text
            color: Theme.textPrimary
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Theme.controlRadius
            color: lutLibraryItem.highlighted
                ? Theme.buttonGhostHover : Theme.transparent
        }
        onTriggered: root.openLutLibraryRequested()
    }

    MenuItem {
        id: cacheMaintenanceItem
        width: root.availableWidth
        height: Theme.controlHeight
        text: qsTr("Cache Maintenance…")
        leftPadding: 35
        contentItem: Label {
            text: cacheMaintenanceItem.text
            color: Theme.textPrimary
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Theme.controlRadius
            color: cacheMaintenanceItem.highlighted
                ? Theme.buttonGhostHover : Theme.transparent
        }
        onTriggered: root.openCacheMaintenanceRequested()
    }

    MenuItem {
        id: mapProviderSettingsItem
        width: root.availableWidth
        height: Theme.controlHeight
        text: qsTr("Map & Location Services…")
        leftPadding: 35
        contentItem: Label {
            text: mapProviderSettingsItem.text
            color: Theme.textPrimary
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Theme.controlRadius
            color: mapProviderSettingsItem.highlighted
                ? Theme.buttonGhostHover : Theme.transparent
        }
        onTriggered: root.openMapProviderSettingsRequested()
    }

    PreferenceSeparator {}

    PreferenceSection {
        text: qsTr("Language")
    }

    PreferenceItem {
        text: qsTr("Follow System")
        checked: root.preferences.languageMode === "system"
        ButtonGroup.group: languageGroup
        onTriggered: root.preferences.languageMode = "system"
    }

    PreferenceItem {
        text: qsTr("English")
        checked: root.preferences.languageMode === "en"
        ButtonGroup.group: languageGroup
        onTriggered: root.preferences.languageMode = "en"
    }

    PreferenceItem {
        text: qsTr("Simplified Chinese")
        checked: root.preferences.languageMode === "zh_CN"
        ButtonGroup.group: languageGroup
        onTriggered: root.preferences.languageMode = "zh_CN"
    }

    contentItem: ListView {
        implicitHeight: contentHeight
        model: root.contentModel
        currentIndex: root.currentIndex
        interactive: false
        clip: true
    }

    background: Item {
        implicitWidth: 236
        implicitHeight: 40

        RectangularShadow {
            x: menuPanel.x
            y: menuPanel.y
            width: menuPanel.width
            height: menuPanel.height
            radius: menuPanel.radius
            color: Theme.shadowStrong
            blur: 18
            spread: 0
            offset.y: 5
            cached: true
        }

        Rectangle {
            id: menuPanel
            anchors.fill: parent
            radius: 11
            color: Theme.panelRaised
            border.width: 1
            border.color: Theme.borderStrong
        }
    }

    enter: Transition {
        NumberAnimation {
            property: "opacity"
            from: 0
            to: 1
            duration: 90
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            property: "scale"
            from: 0.98
            to: 1
            duration: 100
            easing.type: Easing.OutCubic
        }
    }

    exit: Transition {
        NumberAnimation {
            property: "opacity"
            from: 1
            to: 0
            duration: 70
            easing.type: Easing.InCubic
        }
    }
}
