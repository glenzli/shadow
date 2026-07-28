pragma ComponentBehavior: Bound
pragma Translator: "EditHistogram"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns analysis-mode selection, vectorscope filters, freshness status, and proxy identity.
Item {
    id: toolbar

    required property var editor
    required property bool beforeView
    required property int scopeMode
    required property int histogramScope
    required property int waveformScope
    required property int paradeScope
    required property int vectorscopeScope
    required property bool skinGuideVisible
    required property bool showingHistogram
    required property bool showingVectorscope
    required property bool pointColorScopeActive
    required property bool hasData
    required property bool updating
    required property bool presentationStale
    required property real hdrHeadroomPixels
    required property string hdrHeadroomText
    required property string hdrHeadroomTooltip
    required property color accentColor
    required property color secondaryTextColor
    required property color mutedTextColor

    signal scopeModeRequested(int mode)
    signal skinGuideVisibleRequested(bool visible)

    function scopeButtonTooltip(mode) {
        switch (mode) {
        case waveformScope:
            return qsTranslate(
                "EditHistogram", "Display-referred luminance waveform")
        case paradeScope:
            return qsTranslate(
                "EditHistogram",
                "Display-referred red, green, and blue parade")
        case vectorscopeScope:
            return qsTranslate(
                "EditHistogram",
                "Display-referred chroma distribution with a diagnostic skin-tone guide")
        default:
            return qsTranslate(
                "EditHistogram",
                "Scene-linear RGB histogram and pre-clamp clipping")
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 9
        anchors.rightMargin: 9
        spacing: 6

        Label {
            objectName: "analysisScopeTitleText"
            text: qsTranslate("EditHistogram", "ANALYSIS")
            color: toolbar.mutedTextColor
            font.pixelSize: 9
            font.weight: Font.DemiBold
            font.letterSpacing: 1.1
        }

        Row {
            spacing: 2

            Repeater {
                model: [
                    {
                        mode: toolbar.histogramScope,
                        label: qsTranslate("EditHistogram", "H")
                    },
                    {
                        mode: toolbar.waveformScope,
                        label: qsTranslate("EditHistogram", "W")
                    },
                    {
                        mode: toolbar.paradeScope,
                        label: qsTranslate("EditHistogram", "RGB")
                    },
                    {
                        mode: toolbar.vectorscopeScope,
                        label: qsTranslate("EditHistogram", "V")
                    }
                ]

                delegate: Rectangle {
                    id: scopeButton

                    required property var modelData
                    readonly property bool selected:
                        toolbar.scopeMode === modelData.mode

                    width: modelData.mode === toolbar.paradeScope ? 23 : 17
                    height: 16
                    radius: 2
                    color: selected
                        ? Qt.rgba(
                            toolbar.accentColor.r,
                            toolbar.accentColor.g,
                            toolbar.accentColor.b,
                            0.2
                        )
                        : "transparent"
                    border.width: selected ? 1 : 0
                    border.color: selected
                        ? toolbar.accentColor : "transparent"

                    Label {
                        anchors.centerIn: parent
                        text: scopeButton.modelData.label
                        color: scopeButton.selected
                            ? toolbar.accentColor : toolbar.mutedTextColor
                        font.pixelSize: 7
                        font.weight: Font.Bold
                        font.letterSpacing: 0.25
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: toolbar.scopeModeRequested(
                            scopeButton.modelData.mode)
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 450
                        ToolTip.text: toolbar.scopeButtonTooltip(
                            scopeButton.modelData.mode)
                    }
                }
            }
        }

        Rectangle {
            visible: toolbar.showingHistogram && toolbar.hasData
            Layout.preferredWidth: 61
            Layout.preferredHeight: 16
            radius: 2
            color: toolbar.hdrHeadroomPixels > 0
                ? Qt.rgba(0.93, 0.64, 0.25, 0.16) : "transparent"
            border.width: toolbar.hdrHeadroomPixels > 0 ? 1 : 0
            border.color: "#e5a34d"

            Label {
                anchors.centerIn: parent
                text: toolbar.hdrHeadroomText
                color: toolbar.hdrHeadroomPixels > 0
                    ? "#e5a34d" : toolbar.mutedTextColor
                font.pixelSize: 7
                font.weight: Font.Bold
                font.letterSpacing: 0.2
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
                ToolTip.visible: containsMouse
                ToolTip.delay: 450
                ToolTip.text: toolbar.hdrHeadroomTooltip
            }
        }

        Rectangle {
            visible: toolbar.showingVectorscope
            Layout.preferredWidth: 35
            Layout.preferredHeight: 16
            radius: 2
            color: toolbar.skinGuideVisible
                ? Qt.rgba(0.91, 0.49, 0.35, 0.18) : "transparent"
            border.width: toolbar.skinGuideVisible ? 1 : 0
            border.color: "#e68b68"

            Label {
                anchors.centerIn: parent
                text: qsTranslate("EditHistogram", "SKIN")
                color: parent.border.width > 0
                    ? "#e68b68" : toolbar.mutedTextColor
                font.pixelSize: 7
                font.weight: Font.Bold
                font.letterSpacing: 0.35
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: toolbar.skinGuideVisibleRequested(
                    !toolbar.skinGuideVisible)
                ToolTip.visible: containsMouse
                ToolTip.delay: 450
                ToolTip.text: toolbar.skinGuideVisible
                    ? qsTranslate(
                        "EditHistogram", "Hide skin-tone guide")
                    : qsTranslate(
                        "EditHistogram", "Show skin-tone guide")
            }
        }

        Row {
            visible: toolbar.showingVectorscope
            spacing: 2

            Rectangle {
                readonly property bool selected:
                    !toolbar.pointColorScopeActive
                width: 29
                height: 16
                radius: 2
                color: selected
                    ? Qt.rgba(
                        toolbar.accentColor.r,
                        toolbar.accentColor.g,
                        toolbar.accentColor.b,
                        0.2
                    )
                    : "transparent"
                border.width: selected ? 1 : 0
                border.color: selected
                    ? toolbar.accentColor : "transparent"

                Label {
                    anchors.centerIn: parent
                    text: qsTranslate("EditHistogram", "IMG")
                    color: parent.selected
                        ? toolbar.accentColor : toolbar.mutedTextColor
                    font.pixelSize: 7
                    font.weight: Font.Bold
                    font.letterSpacing: 0.25
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked:
                        toolbar.editor.pointColorScopeActive = false
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 450
                    ToolTip.text:
                        qsTranslate(
                            "EditHistogram",
                            "Show the complete image in the vectorscope")
                }
            }

            Rectangle {
                readonly property bool available: !toolbar.beforeView
                    && toolbar.editor.pointColorScopeAvailable
                readonly property bool selected:
                    toolbar.pointColorScopeActive
                width: 35
                height: 16
                radius: 2
                opacity: available ? 1.0 : 0.45
                color: selected
                    ? Qt.rgba(
                        toolbar.accentColor.r,
                        toolbar.accentColor.g,
                        toolbar.accentColor.b,
                        0.2
                    )
                    : "transparent"
                border.width: selected ? 1 : 0
                border.color: selected
                    ? toolbar.accentColor : "transparent"

                Label {
                    anchors.centerIn: parent
                    text: qsTranslate("EditHistogram", "POINT")
                    color: parent.selected
                        ? toolbar.accentColor : toolbar.mutedTextColor
                    font.pixelSize: 7
                    font.weight: Font.Bold
                    font.letterSpacing: 0.2
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: parent.available
                    cursorShape: enabled
                        ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked:
                        toolbar.editor.pointColorScopeActive = true
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 450
                    ToolTip.text: parent.available
                        ? qsTranslate(
                            "EditHistogram",
                            "Filter the vectorscope by the selected Point Color range")
                        : qsTranslate(
                            "EditHistogram",
                            "Select an enabled Point Color to filter the vectorscope")
                }
            }
        }

        Item {
            Layout.fillWidth: true
        }

        Rectangle {
            visible: toolbar.updating || toolbar.presentationStale
            Layout.preferredWidth: 5
            Layout.preferredHeight: 5
            radius: 3
            color: toolbar.updating
                ? toolbar.accentColor : Theme.textPlaceholder
        }

        Label {
            visible: toolbar.updating || toolbar.presentationStale
            text: toolbar.updating
                ? qsTranslate("EditHistogram", "UPDATING")
                : qsTranslate("EditHistogram", "STALE")
            color: toolbar.updating
                ? toolbar.accentColor : toolbar.mutedTextColor
            font.pixelSize: 7
            font.weight: Font.Bold
            font.letterSpacing: 0.6
        }

        Label {
            text: toolbar.beforeView
                ? qsTranslate("EditHistogram", "BEFORE · WARM PROXY")
                : qsTranslate("EditHistogram", "CURRENT · WARM PROXY")
            color: toolbar.beforeView
                ? toolbar.accentColor : toolbar.secondaryTextColor
            font.pixelSize: 7
            font.weight: Font.Bold
            font.letterSpacing: 0.55
        }
    }
}
