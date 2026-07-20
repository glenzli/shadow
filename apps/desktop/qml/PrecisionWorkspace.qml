pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: precision

    required property var editor
    property int selectedNode: 0
    property bool layerExpanded: true
    property real zoomFactor: 1.0

    readonly property color panel: "#121519"
    readonly property color panelRaised: "#181c21"
    readonly property color border: "#2a3037"
    readonly property color textPrimary: "#edf0f2"
    readonly property color textSecondary: "#bdc4ca"
    readonly property color textMuted: "#8b949e"
    readonly property color accent: "#d8b36a"
    readonly property var nodeTitles: ["Exposure", "Contrast", "RGB Channel Gain", "Saturation"]
    readonly property var nodeDescriptions: [
        "Scene-linear exposure in stops",
        "Pivot contrast in the tone stage",
        "Independent creative RGB gains",
        "Luma-preserving color intensity"
    ]

    function resetView() {
        zoomFactor = 1.0
        previewFlick.contentX = 0
        previewFlick.contentY = 0
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: precision.visible && precision.editor.active
            && precision.editor.canUndo && !precision.editor.stateBusy
        onActivated: precision.editor.undo()
    }

    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: precision.visible && precision.editor.active
            && precision.editor.canRedo && !precision.editor.stateBusy
        onActivated: precision.editor.redo()
    }

    ListModel {
        id: nodeModel
        ListElement { nodeTitle: "Exposure"; nodeStage: "SCENE LINEAR" }
        ListElement { nodeTitle: "Contrast"; nodeStage: "TONE" }
        ListElement { nodeTitle: "RGB Channel Gain"; nodeStage: "CREATIVE COLOR" }
        ListElement { nodeTitle: "Saturation"; nodeStage: "CREATIVE COLOR" }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: Math.max(220, Math.min(252, precision.width * 0.19))
            Layout.fillHeight: true
            color: precision.panel
            border.color: precision.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                Label {
                    text: "ADJUSTMENT LAYERS"
                    color: precision.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.5
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    radius: 4
                    color: "#20252b"
                    border.color: "#343b43"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 10
                        spacing: 8

                        Label {
                            text: precision.layerExpanded ? "▾" : "▸"
                            color: precision.textMuted
                            font.pixelSize: 12
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                text: "Basic Adjustments"
                                color: precision.textPrimary
                                font.pixelSize: 12
                                font.weight: Font.Medium
                            }
                            Label {
                                text: "PHOTO SCOPE · ORDERED"
                                color: precision.accent
                                font.pixelSize: 8
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.7
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: precision.layerExpanded = !precision.layerExpanded
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    visible: precision.layerExpanded

                    Repeater {
                        model: nodeModel

                        delegate: Item {
                            id: nodeRow
                            required property int index
                            required property string nodeTitle
                            required property string nodeStage

                            Layout.fillWidth: true
                            Layout.preferredHeight: 58

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                radius: 4
                                color: precision.selectedNode === nodeRow.index ? "#242a30" : "transparent"

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 8
                                    spacing: 10

                                    Rectangle {
                                        Layout.preferredWidth: 22
                                        Layout.preferredHeight: 22
                                        radius: 11
                                        color: precision.selectedNode === nodeRow.index
                                            ? precision.accent : "#2a3037"
                                        Label {
                                            anchors.centerIn: parent
                                            text: String(nodeRow.index + 1).padStart(2, "0")
                                            color: precision.selectedNode === nodeRow.index
                                                ? "#17130d" : precision.textMuted
                                            font.pixelSize: 8
                                            font.weight: Font.Bold
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            Layout.fillWidth: true
                                            text: nodeRow.nodeTitle
                                            color: precision.textPrimary
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: nodeRow.nodeStage
                                            color: precision.textMuted
                                            font.pixelSize: 8
                                            font.letterSpacing: 0.6
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: {
                                        precision.selectedNode = nodeRow.index
                                        rightTabs.currentIndex = 0
                                    }
                                }
                            }

                            Rectangle {
                                visible: nodeRow.index < nodeModel.count - 1
                                x: 34
                                y: 44
                                width: 1
                                height: 26
                                color: "#3a4149"
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: "The layer is the reusable unit. Nodes below it are evaluated in a fixed, deterministic order."
                    color: "#66717c"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    lineHeight: 1.35
                    topPadding: 8
                }

                Item { Layout.fillHeight: true }

                Label {
                    text: "NON-DESTRUCTIVE"
                    color: "#64707b"
                    font.pixelSize: 9
                    font.letterSpacing: 1.1
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#090b0d"

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 46
                    color: "#101317"
                    border.color: precision.border

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
                                text: precision.editor.active ? precision.editor.title : "No photo open"
                                color: precision.textPrimary
                                font.pixelSize: 12
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: precision.editor.sourcePath
                                color: precision.textMuted
                                font.pixelSize: 8
                                elide: Text.ElideMiddle
                            }
                        }

                        Button {
                            id: undoButton
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 27
                            text: "UNDO"
                            enabled: precision.editor.active && precision.editor.canUndo
                                && !precision.editor.stateBusy
                            onClicked: precision.editor.undo()
                            background: Rectangle {
                                radius: 3
                                color: undoButton.down ? "#292f35" : "#1b2025"
                                border.color: undoButton.enabled ? "#48515a" : precision.border
                            }
                            contentItem: Label {
                                text: undoButton.text
                                color: undoButton.enabled ? precision.textPrimary : "#606a74"
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        Button {
                            id: redoButton
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 27
                            text: "REDO"
                            enabled: precision.editor.active && precision.editor.canRedo
                                && !precision.editor.stateBusy
                            onClicked: precision.editor.redo()
                            background: Rectangle {
                                radius: 3
                                color: redoButton.down ? "#292f35" : "#1b2025"
                                border.color: redoButton.enabled ? "#48515a" : precision.border
                            }
                            contentItem: Label {
                                text: redoButton.text
                                color: redoButton.enabled ? precision.textPrimary : "#606a74"
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Label {
                            text: Math.round(precision.zoomFactor * 100) + "%"
                            color: precision.textMuted
                            font.family: "Menlo"
                            font.pixelSize: 9
                        }
                        Slider {
                            id: zoomSlider
                            Layout.preferredWidth: 112
                            from: 1.0
                            to: 4.0
                            stepSize: 0.05
                            value: precision.zoomFactor
                            onMoved: precision.zoomFactor = value
                        }
                        Button {
                            id: fitButton
                            Layout.preferredWidth: 46
                            Layout.preferredHeight: 27
                            text: "FIT"
                            onClicked: precision.resetView()
                            background: Rectangle {
                                radius: 3
                                color: fitButton.down ? "#292f35" : "#1b2025"
                                border.color: precision.border
                            }
                            contentItem: Label {
                                text: fitButton.text
                                color: precision.textMuted
                                font.pixelSize: 9
                                font.weight: Font.Bold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }

                Flickable {
                    id: previewFlick
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    contentWidth: width * precision.zoomFactor
                    contentHeight: height * precision.zoomFactor
                    interactive: precision.zoomFactor > 1.0

                    Image {
                        id: editedPreview
                        width: previewFlick.contentWidth
                        height: previewFlick.contentHeight
                        source: precision.editor.previewSource
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                        smooth: true
                    }

                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: 14
                visible: precision.editor.busy || editedPreview.status === Image.Loading
                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    running: parent.visible
                }
                Label {
                    text: precision.editor.active ? "Rendering local edit" : "Opening photo"
                    color: precision.textPrimary
                    font.pixelSize: 12
                }
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(390, parent.width - 60)
                spacing: 10
                visible: !precision.editor.busy
                    && (!precision.editor.active || editedPreview.status === Image.Error)
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: editedPreview.status === Image.Error ? "PREVIEW ERROR" : "NO PHOTO OPEN"
                    color: editedPreview.status === Image.Error ? "#d28e82" : precision.textMuted
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.2
                }
                Label {
                    width: parent.width
                    text: precision.editor.statusText
                    color: precision.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    lineHeight: 1.35
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: Math.max(304, Math.min(348, precision.width * 0.24))
            Layout.fillHeight: true
            color: precision.panel
            border.color: precision.border

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                TabBar {
                    id: rightTabs
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    background: Rectangle { color: "#101317" }

                    TabButton {
                        id: adjustTab
                        text: "ADJUST"
                        background: Rectangle {
                            color: "transparent"
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 2
                                color: adjustTab.checked ? precision.accent : "transparent"
                            }
                        }
                        contentItem: Label {
                            text: adjustTab.text
                            color: adjustTab.checked ? precision.accent : precision.textMuted
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.1
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    TabButton {
                        id: versionsTab
                        text: "VERSIONS"
                        background: Rectangle {
                            color: "transparent"
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 2
                                color: versionsTab.checked ? precision.accent : "transparent"
                            }
                        }
                        contentItem: Label {
                            text: versionsTab.text
                            color: versionsTab.checked ? precision.accent : precision.textMuted
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.1
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: rightTabs.currentIndex

                    Item {
                        ScrollView {
                            anchors.fill: parent
                            clip: true
                            contentWidth: availableWidth

                            ColumnLayout {
                                width: parent.width
                                spacing: 12
                                enabled: precision.editor.active && !precision.editor.stateBusy

                                Item { Layout.preferredHeight: 4 }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    text: precision.nodeTitles[precision.selectedNode]
                                    color: precision.textPrimary
                                    font.pixelSize: 16
                                    font.weight: Font.Medium
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    text: precision.nodeDescriptions[precision.selectedNode]
                                    color: precision.textMuted
                                    font.pixelSize: 10
                                    wrapMode: Text.WordWrap
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    Layout.preferredHeight: 1
                                    color: precision.border
                                }

                                ShadowSlider {
                                    visible: precision.selectedNode === 0
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    label: "Exposure"
                                    from: -5.0
                                    to: 5.0
                                    stepSize: 0.05
                                    value: precision.editor.exposureStops
                                    suffix: " EV"
                                    onGestureStarted: precision.editor.beginParameterEdit("exposure")
                                    onEdited: value => precision.editor.exposureStops = value
                                    onGestureFinished: precision.editor.endParameterEdit("exposure")
                                }

                                ShadowSlider {
                                    visible: precision.selectedNode === 1
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    label: "Contrast factor"
                                    from: 0.25
                                    to: 2.5
                                    stepSize: 0.01
                                    value: precision.editor.contrastFactor
                                    suffix: "×"
                                    onGestureStarted: precision.editor.beginParameterEdit("contrast")
                                    onEdited: value => precision.editor.contrastFactor = value
                                    onGestureFinished: precision.editor.endParameterEdit("contrast")
                                }

                                ShadowSlider {
                                    visible: precision.selectedNode === 2
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    label: "Red gain"
                                    from: 0.25
                                    to: 2.5
                                    stepSize: 0.01
                                    value: precision.editor.redGain
                                    suffix: "×"
                                    onGestureStarted: precision.editor.beginParameterEdit("red_gain")
                                    onEdited: value => precision.editor.redGain = value
                                    onGestureFinished: precision.editor.endParameterEdit("red_gain")
                                }
                                ShadowSlider {
                                    visible: precision.selectedNode === 2
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    label: "Green gain"
                                    from: 0.25
                                    to: 2.5
                                    stepSize: 0.01
                                    value: precision.editor.greenGain
                                    suffix: "×"
                                    onGestureStarted: precision.editor.beginParameterEdit("green_gain")
                                    onEdited: value => precision.editor.greenGain = value
                                    onGestureFinished: precision.editor.endParameterEdit("green_gain")
                                }
                                ShadowSlider {
                                    visible: precision.selectedNode === 2
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    label: "Blue gain"
                                    from: 0.25
                                    to: 2.5
                                    stepSize: 0.01
                                    value: precision.editor.blueGain
                                    suffix: "×"
                                    onGestureStarted: precision.editor.beginParameterEdit("blue_gain")
                                    onEdited: value => precision.editor.blueGain = value
                                    onGestureFinished: precision.editor.endParameterEdit("blue_gain")
                                }

                                ShadowSlider {
                                    visible: precision.selectedNode === 3
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    label: "Saturation"
                                    from: 0.0
                                    to: 2.5
                                    stepSize: 0.01
                                    value: precision.editor.saturationFactor
                                    suffix: "×"
                                    onGestureStarted: precision.editor.beginParameterEdit("saturation")
                                    onEdited: value => precision.editor.saturationFactor = value
                                    onGestureFinished: precision.editor.endParameterEdit("saturation")
                                }

                                Item { Layout.preferredHeight: 8 }

                                Button {
                                    id: resetButton
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    Layout.preferredHeight: 36
                                    text: "RESET TO NEUTRAL"
                                    enabled: precision.editor.active && !precision.editor.stateBusy
                                    onClicked: precision.editor.resetEdits()
                                    background: Rectangle {
                                        radius: 4
                                        color: resetButton.down ? "#2a3037" : "#1b2025"
                                        border.color: precision.border
                                    }
                                    contentItem: Label {
                                        text: resetButton.text
                                        color: precision.textPrimary
                                        font.pixelSize: 9
                                        font.weight: Font.DemiBold
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }

                                Button {
                                    id: revertButton
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 18
                                    Layout.rightMargin: 18
                                    Layout.preferredHeight: 36
                                    text: "REVERT TO SAVED VERSION"
                                    enabled: precision.editor.active && precision.editor.dirty
                                        && !precision.editor.stateBusy
                                    onClicked: precision.editor.revertEdits()
                                    background: Rectangle {
                                        radius: 4
                                        color: revertButton.down ? "#352c20" : "#211d18"
                                        border.color: revertButton.enabled ? "#5d4b2d" : precision.border
                                    }
                                    contentItem: Label {
                                        text: revertButton.text
                                        color: revertButton.enabled ? precision.accent : "#606a74"
                                        font.pixelSize: 9
                                        font.weight: Font.DemiBold
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }

                                Item { Layout.preferredHeight: 14 }
                            }
                        }
                    }

                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 18
                            spacing: 10

                            Label {
                                text: "SAVE CURRENT LOOK"
                                color: precision.textMuted
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1.2
                            }

                            TextField {
                                id: versionLabel
                                Layout.fillWidth: true
                                Layout.preferredHeight: 38
                                enabled: precision.editor.active && !precision.editor.stateBusy
                                placeholderText: "Version name"
                                color: precision.textPrimary
                                placeholderTextColor: "#68727c"
                                selectByMouse: true
                                background: Rectangle {
                                    radius: 4
                                    color: "#181c21"
                                    border.color: versionLabel.activeFocus ? precision.accent : precision.border
                                }
                                onAccepted: {
                                    const cleanLabel = text.trim()
                                    if (cleanLabel.length > 0 && saveButton.enabled) {
                                        precision.editor.saveVersion(cleanLabel)
                                        clear()
                                    }
                                }
                            }

                            Button {
                                id: saveButton
                                Layout.fillWidth: true
                                Layout.preferredHeight: 38
                                text: precision.editor.stateBusy ? "SAVING…" : "SAVE VERSION"
                                enabled: precision.editor.active && !precision.editor.stateBusy
                                    && versionLabel.text.trim().length > 0
                                onClicked: {
                                    precision.editor.saveVersion(versionLabel.text.trim())
                                    versionLabel.clear()
                                }
                                background: Rectangle {
                                    radius: 4
                                    color: saveButton.enabled
                                        ? (saveButton.down ? "#b9914e" : precision.accent)
                                        : "#252a30"
                                }
                                contentItem: Label {
                                    text: saveButton.text
                                    color: saveButton.enabled ? "#17130d" : "#606a74"
                                    font.pixelSize: 10
                                    font.weight: Font.Bold
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                color: precision.border
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: "HISTORY"
                                    color: precision.textMuted
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 1.2
                                }
                                Label {
                                    text: versionList.count
                                    color: precision.textMuted
                                    font.pixelSize: 9
                                }
                            }

                            ListView {
                                id: versionList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                model: precision.editor.versions
                                spacing: 7
                                clip: true

                                delegate: Rectangle {
                                    id: versionRow
                                    required property string commitId
                                    required property string label
                                    required property string createdAtText
                                    required property bool current
                                    required property int parentCount
                                    required property string changeSummary
                                    required property string parentSummary

                                    width: versionList.width
                                    height: 78
                                    radius: 4
                                    color: current ? "#242820" : "#181c21"
                                    border.color: current ? "#625334" : precision.border

                                    Column {
                                        anchors.left: parent.left
                                        anchors.right: currentBadge.left
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.leftMargin: 11
                                        anchors.rightMargin: 8
                                        spacing: 4
                                        Label {
                                            width: parent.width
                                            text: versionRow.label
                                            color: precision.textPrimary
                                            font.pixelSize: 11
                                            font.weight: Font.Medium
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            width: parent.width
                                            text: versionRow.changeSummary
                                            color: precision.textSecondary
                                            font.pixelSize: 10
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            width: parent.width
                                            text: versionRow.createdAtText + "  ·  "
                                                + versionRow.parentSummary
                                            color: precision.textMuted
                                            font.pixelSize: 9
                                            elide: Text.ElideRight
                                        }
                                    }

                                    Label {
                                        id: currentBadge
                                        anchors.right: parent.right
                                        anchors.rightMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: versionRow.current ? "CURRENT" : "CHECKOUT"
                                        color: versionRow.current ? precision.accent : precision.textMuted
                                        font.pixelSize: 8
                                        font.weight: Font.Bold
                                        font.letterSpacing: 0.7
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: !versionRow.current && !precision.editor.stateBusy
                                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                        onClicked: precision.editor.checkoutVersion(versionRow.commitId)
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    width: parent.width - 20
                                    visible: versionList.count === 0
                                    text: "Saved looks will appear here. Checking one out changes the working edit without deleting newer versions."
                                    color: precision.textMuted
                                    font.pixelSize: 10
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                    lineHeight: 1.35
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
