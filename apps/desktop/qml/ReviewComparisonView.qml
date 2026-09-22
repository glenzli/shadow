pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Ordinary comparison is a repeatable, non-mutating 1:1 viewing surface.
// Both panes navigate the current filtered Library order independently.
Rectangle {
    id: comparisonView

    required property var review
    property bool linkedViews: true

    function synchronizeView(paneIndex, transform) {
        if (!linkedViews)
            return
        const otherPane = comparisonPanes.itemAt(1 - paneIndex)
        if (otherPane)
            otherPane.viewport.applyView(transform)
    }

    anchors.fill: parent
    anchors.margins: 18
    visible: review.comparison.compareMode
    enabled: visible
    focus: visible
    z: 5
    radius: Theme.controlRadius
    color: Theme.window
    border.width: 1
    border.color: review.border

    onVisibleChanged: {
        if (visible)
            forceActiveFocus()
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape) {
            review.comparison.exitComparison()
        } else {
            return
        }
        event.accepted = true
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: qsTr("PHOTO COMPARISON")
                    color: comparisonView.review.textPrimary
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.0
                }

                Label {
                    text: comparisonView.review.comparison.groupMode
                        ? qsTr("Similar group · %L1 photos · arrows stay within this group. Check each photo before deciding.")
                            .arg(comparisonView.review.comparison.groupCount)
                        : qsTr("Preview comparison · double-click to zoom, drag to pan. Use Precision for original detail.")
                    color: comparisonView.review.textMuted
                    font.pixelSize: Theme.fontMeta
                }
            }

            ShadowCheckBox {
                objectName: "comparisonLinkViews"
                text: qsTr("Link zoom and pan")
                checked: comparisonView.linkedViews
                onToggled: {
                    comparisonView.linkedViews = checked
                    const leftPane = comparisonPanes.itemAt(0)
                    if (checked && leftPane)
                        comparisonView.synchronizeView(0, leftPane.viewport.currentView())
                }
            }

            ShadowIconButton {
                source: "qrc:/icons/swap.svg"
                toolTipText: qsTr("Swap the left and right photos")
                accessibleName: toolTipText
                onClicked: comparisonView.review.comparison.swapPanes()
            }

            ShadowIconButton {
                source: "qrc:/icons/clear.svg"
                toolTipText: qsTr("Exit comparison (Esc)")
                accessibleName: toolTipText
                onClicked: comparisonView.review.comparison.exitComparison()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            Repeater {
                id: comparisonPanes
                model: [0, 1]

                delegate: Rectangle {
                    id: comparisonPane

                    required property int modelData
                    readonly property int paneIndex: modelData
                    property alias viewport: comparisonViewport
                    readonly property var snapshot: paneIndex === 0
                        ? comparisonView.review.comparison.leftComparisonSnapshot
                        : comparisonView.review.comparison.rightComparisonSnapshot

                    objectName: paneIndex === 0
                        ? "ordinaryComparisonLeftPane"
                        : "ordinaryComparisonRightPane"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    Layout.minimumWidth: 0
                    radius: Theme.controlRadius
                    color: comparisonView.review.panelRaised
                    border.width: 1
                    border.color: comparisonView.review.border

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 5

                            ShadowButton {
                                compact: true
                                minimumButtonWidth: 34
                                text: qsTr("‹")
                                toolTipText: comparisonView.review.comparison.groupMode
                                    ? qsTr("Previous photo in this group")
                                    : qsTr("Previous photo in this pane")
                                onClicked: comparisonView.review.comparison
                                    .navigatePane(comparisonPane.paneIndex, -1)
                            }

                            Label {
                                Layout.fillWidth: true
                                text: comparisonPane.snapshot
                                    ? String(comparisonPane.snapshot.title || "") : ""
                                color: comparisonView.review.textPrimary
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideMiddle
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.Medium
                            }

                            ShadowButton {
                                compact: true
                                minimumButtonWidth: 34
                                text: qsTr("›")
                                toolTipText: comparisonView.review.comparison.groupMode
                                    ? qsTr("Next photo in this group")
                                    : qsTr("Next photo in this pane")
                                onClicked: comparisonView.review.comparison
                                    .navigatePane(comparisonPane.paneIndex, 1)
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 220
                            color: Theme.comparisonCanvas
                            border.width: 1
                            border.color: Theme.imageBorder
                            clip: true

                            ReviewPreviewViewport {
                                id: comparisonViewport
                                objectName: comparisonPane.paneIndex === 0
                                    ? "ordinaryComparisonLeftImage"
                                    : "ordinaryComparisonRightImage"
                                anchors.fill: parent
                                anchors.margins: 1
                                source: comparisonPane.snapshot
                                    ? String(comparisonPane.snapshot.visualSource || "")
                                    : ""
                                autoTransform: false
                                selectionKey: source.toString()
                                onViewChanged: transform => comparisonView.synchronizeView(
                                    comparisonPane.paneIndex, transform)
                            }

                            Label {
                                anchors.centerIn: parent
                                visible: !comparisonViewport.imageReady
                                text: comparisonViewport.imageFailed
                                    ? qsTr("IMAGE LOAD FAILED") : qsTr("LOADING PHOTO…")
                                color: comparisonViewport.imageFailed
                                    ? Theme.errorText : comparisonView.review.textMuted
                                font.pixelSize: Theme.fontMeta
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: comparisonView.review.comparison
                .localComparisonStatus.length > 0
            text: comparisonView.review.comparison.localComparisonStatus
            color: Theme.warningText
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontMeta
        }
    }
}
