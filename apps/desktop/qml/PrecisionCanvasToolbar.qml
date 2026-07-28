pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Presents the precision viewport's title and command affordances. It emits
// intent only; comparison, zebra, and viewport state remain owned by the canvas.
Rectangle {
    id: toolbar

    Layout.fillWidth: true
    Layout.preferredHeight: 42
    color: Theme.chrome
    border.width: 0

    required property var editor
    required property bool zebraEnabled
    required property bool comparisonActive
    required property int comparisonMode
    required property int comparisonWhole
    required property int comparisonWipeVertical
    required property int comparisonWipeHorizontal
    required property int comparisonSideBySide
    required property int comparisonStacked
    required property bool fitView
    required property real zoomFactor

    signal zebraToggleRequested()
    signal comparisonDisableRequested()
    signal comparisonModeRequested(int mode)
    signal zoomRequested(real value)
    signal fitRequested()

    function comparisonModeName(mode) {
        if (mode === comparisonWhole)
            return qsTr("Original only")
        if (mode === comparisonWipeVertical)
            return qsTr("Vertical wipe")
        if (mode === comparisonWipeHorizontal)
            return qsTr("Horizontal wipe")
        if (mode === comparisonSideBySide)
            return qsTr("Side by side")
        return qsTr("Top and bottom")
    }

    function comparisonModeIcon(mode) {
        if (mode === comparisonWipeVertical)
            return "qrc:/icons/compare-wipe-vertical.svg"
        if (mode === comparisonWipeHorizontal)
            return "qrc:/icons/compare-wipe-horizontal.svg"
        if (mode === comparisonSideBySide)
            return "qrc:/icons/compare-side-by-side.svg"
        if (mode === comparisonStacked)
            return "qrc:/icons/compare-stacked.svg"
        return "qrc:/icons/before-after.svg"
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 12
        spacing: 10

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Label {
                Layout.fillWidth: true
                text: toolbar.editor.active
                    ? toolbar.editor.title : qsTr("No photo open")
                color: Theme.textPrimary
                font.pixelSize: 12
                font.weight: Font.Medium
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: toolbar.editor.sourcePath
                color: Theme.textMuted
                font.pixelSize: 8
                elide: Text.ElideMiddle
            }
        }

        RowLayout {
            spacing: 2

            ShadowIconButton {
                id: zebraButton
                source: "qrc:/icons/zebra.svg"
                variant: ShadowIconButton.Secondary
                selected: toolbar.zebraEnabled
                toolTipText: qsTr(
                    "Toggle clipping warning · RAW uses sensor limits")
                accessibleName: toolTipText
                Accessible.checked: selected
                enabled: toolbar.editor.active
                onClicked: toolbar.zebraToggleRequested()
            }

            ShadowIconButton {
                id: beforeAfterButton
                source: toolbar.comparisonModeIcon(toolbar.comparisonMode)
                variant: ShadowIconButton.Secondary
                selected: toolbar.comparisonActive
                toolTipText: toolbar.comparisonActive
                    ? qsTr("Disable comparison")
                    : qsTr("Compare with original · %1").arg(
                        toolbar.comparisonModeName(toolbar.comparisonMode))
                accessibleName: toolTipText
                Accessible.checked: selected
                enabled: toolbar.editor.active
                onClicked: {
                    if (toolbar.comparisonActive)
                        toolbar.comparisonDisableRequested()
                    else
                        toolbar.comparisonModeRequested(toolbar.comparisonMode)
                }
            }

            ShadowIconButton {
                id: comparisonModeButton
                source: "qrc:/icons/chevron-down.svg"
                buttonSize: 24
                iconSize: 12
                toolTipText: qsTr("Choose comparison layout")
                accessibleName: toolTipText
                enabled: toolbar.editor.active
                onClicked: comparisonModePopup.open()

                Popup {
                    id: comparisonModePopup
                    parent: comparisonModeButton
                    x: Math.round((comparisonModeButton.width - width) / 2)
                    y: comparisonModeButton.height + 6
                    width: comparisonModeRow.implicitWidth + 16
                    height: comparisonModeRow.implicitHeight + 16
                    padding: 8
                    modal: false
                    closePolicy: Popup.CloseOnEscape
                        | Popup.CloseOnPressOutside

                    background: Rectangle {
                        radius: Theme.controlRadius
                        color: Theme.panelRaised
                        border.width: 1
                        border.color: Theme.borderStrong
                    }

                    contentItem: Row {
                        id: comparisonModeRow
                        spacing: 4

                        Repeater {
                            model: [
                                { "mode": toolbar.comparisonWhole,
                                  "icon": "qrc:/icons/before-after.svg" },
                                { "mode": toolbar.comparisonWipeVertical,
                                  "icon": "qrc:/icons/compare-wipe-vertical.svg" },
                                { "mode": toolbar.comparisonWipeHorizontal,
                                  "icon": "qrc:/icons/compare-wipe-horizontal.svg" },
                                { "mode": toolbar.comparisonSideBySide,
                                  "icon": "qrc:/icons/compare-side-by-side.svg" },
                                { "mode": toolbar.comparisonStacked,
                                  "icon": "qrc:/icons/compare-stacked.svg" }
                            ]

                            delegate: ShadowIconButton {
                                required property var modelData
                                source: modelData.icon
                                variant: ShadowIconButton.Secondary
                                selected: toolbar.comparisonMode
                                    === modelData.mode
                                toolTipText: toolbar.comparisonModeName(
                                    modelData.mode)
                                accessibleName: toolTipText
                                onClicked: {
                                    comparisonModePopup.close()
                                    toolbar.comparisonModeRequested(
                                        modelData.mode)
                                }
                            }
                        }
                    }
                }
            }
        }

        Label {
            text: toolbar.fitView
                ? qsTr("FIT")
                : qsTr("%L1%").arg(Math.round(toolbar.zoomFactor * 100))
            color: Theme.textMuted
            font.family: "Menlo"
            font.pixelSize: 9
        }

        ShadowInlineSlider {
            id: zoomSlider
            Layout.preferredWidth: 112
            Layout.minimumWidth: 72
            from: 0.25
            to: 4.0
            stepSize: 0.05
            value: toolbar.zoomFactor
            enabled: toolbar.editor.active && !toolbar.editor.stateBusy
            onMoved: toolbar.zoomRequested(value)
        }

        ShadowButton {
            id: actualPixelsButton
            Layout.preferredWidth: 52
            Layout.preferredHeight: 30
            compact: true
            variant: ShadowButton.Secondary
            selected: !toolbar.fitView
                && Math.abs(toolbar.zoomFactor - 1.0) < 0.001
            text: qsTr("100%")
            enabled: toolbar.editor.active
            onClicked: toolbar.zoomRequested(1.0)
        }

        ShadowIconButton {
            id: fitButton
            source: "qrc:/icons/fit-view.svg"
            variant: ShadowIconButton.Secondary
            selected: toolbar.fitView
            toolTipText: qsTr("Fit image to window")
            accessibleName: toolTipText
            Accessible.checked: selected
            enabled: toolbar.editor.active && !toolbar.editor.stateBusy
            onClicked: toolbar.fitRequested()
        }
    }
}
