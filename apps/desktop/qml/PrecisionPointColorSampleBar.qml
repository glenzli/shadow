pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Point Color collection navigation plus one-shot sample admission. Range
// adjustment and Skin Check stay with PrecisionPointColorSection.
ColumnLayout {
    id: sampleBar
    objectName: "pointColorSampleBar"

    required property var editor
    required property bool pickerAvailable
    required property color accent

    readonly property int swatchTargetSize: 36
    readonly property int swatchSpacing: 6
    readonly property int focusRingInset: 2
    readonly property int selectedSamplePosition: {
        for (let itemIndex = 0;
                itemIndex < editor.pointColors.length; ++itemIndex) {
            if (Number(editor.pointColors[itemIndex].index)
                    === editor.selectedPointColorIndex) {
                return itemIndex
            }
        }
        return -1
    }

    Layout.fillWidth: true
    Layout.leftMargin: 14
    Layout.rightMargin: 14
    Layout.topMargin: 7
    Layout.bottomMargin: 8
    spacing: 7

    function revealSelectedSwatch() {
        swatchFlickable.ensurePositionVisible(selectedSamplePosition)
    }

    function revealFocusedOrSelectedSwatch() {
        // Layout and parameter updates may queue this before keyboard focus
        // moves. Resolve current focus now so a stale selection reveal cannot
        // scroll the focused delegate out of view.
        for (let position = 0; position < swatchRepeater.count; ++position) {
            const swatch = swatchRepeater.itemAt(position)
            if (swatch && swatch.activeFocus) {
                swatchFlickable.ensurePositionVisible(position)
                return
            }
        }
        revealSelectedSwatch()
    }

    function queueRevealSelectedSwatch() {
        Qt.callLater(sampleBar.revealFocusedOrSelectedSwatch)
    }

    Component.onCompleted: queueRevealSelectedSwatch()

    Connections {
        target: sampleBar.editor

        function onParametersChanged() {
            sampleBar.queueRevealSelectedSwatch()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Flickable {
            id: swatchFlickable
            objectName: "pointColorSwatchFlickable"

            Layout.fillWidth: true
            Layout.preferredHeight: sampleBar.swatchTargetSize
            contentWidth: sampleBar.editor.pointColors.length > 0
                ? sampleBar.editor.pointColors.length
                    * sampleBar.swatchTargetSize
                    + (sampleBar.editor.pointColors.length - 1)
                        * sampleBar.swatchSpacing
                : 0
            contentHeight: height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.HorizontalFlick

            function ensurePositionVisible(position) {
                if (position < 0 || width <= 0)
                    return
                const itemLeft = position * (
                    sampleBar.swatchTargetSize + sampleBar.swatchSpacing)
                const itemRight = itemLeft + sampleBar.swatchTargetSize
                const maximumContentX = Math.max(0, contentWidth - width)
                if (itemLeft < contentX) {
                    contentX = Math.max(0, itemLeft)
                } else if (itemRight > contentX + width) {
                    contentX = Math.min(
                        maximumContentX,
                        itemRight - width
                    )
                }
            }

            onWidthChanged: sampleBar.queueRevealSelectedSwatch()

            Row {
                id: swatchRow

                height: parent.height
                spacing: sampleBar.swatchSpacing

                Repeater {
                    id: swatchRepeater
                    objectName: "pointColorSwatchRepeater"
                    model: sampleBar.editor.pointColors

                    delegate: Button {
                        id: pointColorSwatch

                        required property var modelData
                        required property int index
                        readonly property int sampleIndex:
                            Number(modelData.index)
                        readonly property int samplePosition: index
                        readonly property bool selected:
                            sampleBar.editor.selectedPointColorIndex
                                === sampleIndex
                        objectName: "pointColorSwatch"

                        width: sampleBar.swatchTargetSize
                        height: sampleBar.swatchTargetSize
                        leftPadding: 0
                        rightPadding: 0
                        topPadding: 0
                        bottomPadding: 0
                        hoverEnabled: enabled
                        focusPolicy: Qt.StrongFocus
                        Accessible.name: qsTr(
                            "Point Color sample %1").arg(
                            sampleIndex + 1)
                        Accessible.description:
                            qsTr("Select this sampled color range")
                        Accessible.selected: selected

                        background: Rectangle {
                            anchors.fill: parent
                            radius: width / 2
                            color: pointColorSwatch.modelData.swatch
                            border.width:
                                pointColorSwatch.selected ? 3 : 1
                            border.color:
                                pointColorSwatch.selected
                                    ? sampleBar.accent : Theme.borderStrong

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins:
                                    sampleBar.focusRingInset
                                visible: pointColorSwatch.visualFocus
                                radius: Math.max(0, width / 2)
                                color: Theme.transparent
                                border.width: 1
                                border.color: Theme.focusRing
                            }
                        }
                        contentItem: Item {}
                        onClicked: sampleBar.editor.selectPointColor(
                            sampleIndex)
                        onActiveFocusChanged: {
                            if (activeFocus) {
                                swatchFlickable.ensurePositionVisible(
                                    samplePosition)
                            }
                        }
                        Component.onCompleted: {
                            if (selected)
                                sampleBar.queueRevealSelectedSwatch()
                        }
                    }
                }
            }
        }

        ShadowIconButton {
            buttonSize: 36
            iconSize: 19
            source: "qrc:/icons/eyedropper-add.svg"
            selected: sampleBar.editor.pointColorPickerActive
            enabled: sampleBar.pickerAvailable
            toolTipText: sampleBar.editor.pointColorPickerActive
                ? qsTr("Cancel Point Color sampling · Esc")
                : qsTr("Add a Point Color sample from the image")
            accessibleName: toolTipText
            onClicked: sampleBar.editor.setPointColorPickerActive(
                !sampleBar.editor.pointColorPickerActive)
        }

        ShadowIconButton {
            buttonSize: 36
            iconSize: 18
            source: "qrc:/icons/trash.svg"
            variant: ShadowIconButton.Danger
            enabled: sampleBar.editor.selectedPointColorIndex >= 0
            toolTipText: qsTr("Remove selected Point Color sample")
            accessibleName: toolTipText
            onClicked: sampleBar.editor.removeSelectedPointColor()
        }
    }

    Label {
        objectName: "pointColorPickerGuidance"
        Layout.fillWidth: true
        text: sampleBar.editor.pointColorPickerActive
            ? qsTr("Click a color in the photo · Esc cancels")
            : sampleBar.editor.selectedPointColorIndex >= 0
                ? qsTr("Selected range is ready to refine.")
                : qsTr("Sample a color from the photo to begin.")
        color: sampleBar.editor.pointColorPickerActive
            ? Theme.accentTextMuted : Theme.textMuted
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.WordWrap
        lineHeight: 1.2
    }
}
