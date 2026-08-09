pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A guided 1:1 duel over one temporary candidate draft. One pass finds the
// top preference tier and the photos that lost directly to it; lower-ranked
// photos are deliberately not forced through a complete ordering.
Rectangle {
    id: arena

    required property var review

    visible: review.culling.arenaActive
    enabled: visible
    focus: visible
    color: Theme.window
    z: 6

    onVisibleChanged: {
        if (visible)
            forceActiveFocus()
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape)
            review.culling.leaveArena()
        else if (!review.culling.arenaComplete
                 && event.key === Qt.Key_Left)
            review.culling.chooseLeft()
        else if (!review.culling.arenaComplete
                 && event.key === Qt.Key_Right)
            review.culling.chooseRight()
        else if (!review.culling.arenaComplete
                 && (event.key === Qt.Key_Equal
                     || event.key === Qt.Key_0))
            review.culling.chooseEqual()
        else
            return
        event.accepted = true
    }

    component ArenaPhotoPane: Rectangle {
        id: photoPane

        required property var snapshot
        required property string sideLabel

        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.border

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            RowLayout {
                Layout.fillWidth: true

                Label {
                    text: photoPane.sideLabel
                    color: Theme.accent
                    font.pixelSize: Theme.fontMeta
                    font.weight: Font.Bold
                }

                Label {
                    Layout.fillWidth: true
                    text: photoPane.snapshot
                        ? String(photoPane.snapshot.title || "") : ""
                    color: Theme.textPrimary
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideMiddle
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 240
                color: Theme.comparisonCanvas
                border.width: 1
                border.color: Theme.imageBorder
                clip: true

                Image {
                    id: arenaImage
                    anchors.fill: parent
                    anchors.margins: 1
                    source: photoPane.snapshot
                        ? String(photoPane.snapshot.visualSource || "") : ""
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: true
                    smooth: true
                    mipmap: true
                    sourceSize.width: 2048
                    sourceSize.height: 2048
                }

                Label {
                    anchors.centerIn: parent
                    visible: arenaImage.status !== Image.Ready
                    text: arenaImage.status === Image.Error
                        ? qsTr("IMAGE LOAD FAILED") : qsTr("LOADING PHOTO…")
                    color: arenaImage.status === Image.Error
                        ? Theme.errorText : Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: arena.review.culling.arenaComplete
                        ? arena.review.culling.refinementRound > 0
                            ? qsTr("RUNNER-UP RESULTS")
                            : qsTr("CANDIDATE RESULTS")
                        : arena.review.culling.refinementRound > 0
                            ? qsTr("RUNNER-UP DUEL")
                            : qsTr("CANDIDATE DUEL")
                    color: Theme.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.0
                }

                Label {
                    text: arena.review.culling.arenaComplete
                        ? qsTr("The top choice and its direct runner-up pool are ready. No photo metadata has changed.")
                        : qsTr("%L1 of %L2 candidates compared · %L3 decisions")
                            .arg(arena.review.culling.rankedCandidateCount)
                            .arg(arena.review.culling.totalArenaCandidateCount)
                            .arg(arena.review.culling.comparisonCount)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }
            }

            ShadowButton {
                objectName: "cullingArenaUndoButton"
                compact: true
                text: qsTr("Undo")
                enabled: arena.review.culling.history.length > 0
                onClicked: arena.review.culling.undoLastChoice()
            }

            ShadowIconButton {
                source: "qrc:/icons/clear.svg"
                toolTipText: qsTr("Leave the candidate duel")
                accessibleName: toolTipText
                onClicked: arena.review.culling.leaveArena()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !arena.review.culling.arenaComplete
            spacing: 12

            ArenaPhotoPane {
                objectName: "cullingArenaLeftPane"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                snapshot: arena.review.culling.currentLeft
                sideLabel: qsTr("CURRENT LEADER")
            }

            ArenaPhotoPane {
                objectName: "cullingArenaRightPane"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                snapshot: arena.review.culling.currentRight
                sideLabel: qsTr("CHALLENGER")
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            visible: !arena.review.culling.arenaComplete
            spacing: 8

            ShadowIconButton {
                objectName: "cullingArenaChooseLeftButton"
                variant: ShadowIconButton.Primary
                source: "qrc:/icons/slot-left.svg"
                toolTipText: qsTr("Choose the left photo (Left Arrow)")
                accessibleName: toolTipText
                onClicked: arena.review.culling.chooseLeft()
            }

            ShadowIconButton {
                objectName: "cullingArenaChooseEqualButton"
                variant: ShadowIconButton.Tinted
                source: "qrc:/icons/tie.svg"
                toolTipText: qsTr("Place both photos in the same preference tier")
                accessibleName: toolTipText
                onClicked: arena.review.culling.chooseEqual()
            }

            ShadowIconButton {
                objectName: "cullingArenaChooseRightButton"
                variant: ShadowIconButton.Primary
                source: "qrc:/icons/slot-right.svg"
                toolTipText: qsTr("Choose the right photo (Right Arrow)")
                accessibleName: toolTipText
                onClicked: arena.review.culling.chooseRight()
            }

            ShadowIconButton {
                objectName: "cullingArenaSkipButton"
                source: "qrc:/icons/skip.svg"
                toolTipText: qsTr("Leave this photo unresolved")
                accessibleName: toolTipText
                onClicked: arena.review.culling.skipCurrent()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: arena.review.culling.arenaComplete
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: parent.width
                spacing: 10

                Repeater {
                    model: arena.review.culling.tiers

                    delegate: Rectangle {
                        id: resultTier

                        required property int index
                        required property var modelData

                        Layout.fillWidth: true
                        Layout.preferredHeight: 128
                        radius: Theme.controlRadius
                        color: index === 0
                            ? Theme.accentSurfaceQuiet : Theme.panelRaised
                        border.width: 1
                        border.color: index === 0
                            ? Theme.accent : Theme.border

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            Label {
                                Layout.preferredWidth: 64
                                text: resultTier.index === 0
                                    ? qsTr("TOP PICKS") : qsTr("RUNNER-UP POOL")
                                color: resultTier.index === 0
                                    ? Theme.accentSelectionText : Theme.textMuted
                                font.pixelSize: Theme.fontMeta
                                font.weight: Font.Bold
                            }

                            Repeater {
                                model: resultTier.modelData

                                delegate: Rectangle {
                                    id: resultPhoto

                                    required property var modelData
                                    Layout.preferredWidth: 150
                                    Layout.fillHeight: true
                                    radius: Theme.compactControlRadius
                                    color: Theme.comparisonCanvas
                                    clip: true

                                    Image {
                                        anchors.fill: parent
                                        source: String(resultPhoto.modelData
                                            ? resultPhoto.modelData.visualSource || "" : "")
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                        cache: true
                                        sourceSize.width: 320
                                        sourceSize.height: 240
                                    }

                                    Rectangle {
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        height: 28
                                        color: Theme.thumbnailCaptionOverlay

                                        Label {
                                            anchors.fill: parent
                                            anchors.leftMargin: 7
                                            anchors.rightMargin: 7
                                            text: String(resultPhoto.modelData
                                                ? resultPhoto.modelData.title || "" : "")
                                            color: Theme.textPrimary
                                            verticalAlignment: Text.AlignVCenter
                                            elide: Text.ElideRight
                                            font.pixelSize: Theme.fontMeta
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: arena.review.culling
                                            .selectResult(resultPhoto.modelData)
                                    }

                                    ToolTip.visible: resultHover.hovered
                                    ToolTip.text: qsTr("Return to this photo in the Library")

                                    HoverHandler { id: resultHover }
                                }
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: arena.review.culling.unresolvedCandidates.length > 0
                    text: qsTr("%L1 skipped candidates remain unresolved.")
                        .arg(arena.review.culling.unresolvedCandidates.length)
                    color: Theme.warningText
                    font.pixelSize: Theme.fontMeta
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            visible: arena.review.culling.arenaComplete
            spacing: 8

            ShadowButton {
                visible: arena.review.culling.canRefineRunnerUps
                text: qsTr("Refine runners-up")
                onClicked: arena.review.culling.refineRunnerUps()
            }

            ShadowButton {
                text: qsTr("Return to Library")
                onClicked: arena.review.culling.leaveArena()
            }

            ShadowButton {
                variant: ShadowButton.Primary
                text: qsTr("View top result")
                onClicked: arena.review.culling.selectTopResult()
            }

            ShadowButton {
                variant: ShadowButton.Danger
                text: qsTr("Clear candidates")
                onClicked: arena.review.culling.clearCandidates()
            }
        }
    }
}
