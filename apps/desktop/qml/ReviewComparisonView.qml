pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Verified A/B evidence has its own lifecycle and presentation. Keeping it
// separate prevents gallery layout changes from touching comparison receipts.
Item {
    id: comparisonView

    required property var review

                anchors.fill: parent
                anchors.margins: 18
                visible: review.comparison.compareMode
                focus: visible

                onVisibleChanged: {
                    if (visible)
                        forceActiveFocus()
                }

                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Escape) {
                        review.comparison.exitComparison()
                    } else if (review.comparison.selectionCompareMode
                               && event.key === Qt.Key_Left) {
                        review.comparison.navigateCandidate(-1)
                    } else if (review.comparison.selectionCompareMode
                               && event.key === Qt.Key_Right) {
                        review.comparison.navigateCandidate(1)
                    } else if (review.comparison.selectionCompareMode
                               && (event.key === Qt.Key_Return
                                   || event.key === Qt.Key_Enter)) {
                        review.comparison.promoteCandidate()
                    } else {
                        return
                    }
                    event.accepted = true
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                text: review.comparison.selectionCompareMode
                                    ? qsTr("ANCHOR · CANDIDATE")
                                    : qsTr("COMPARE EVIDENCE")
                                color: review.textPrimary
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1.1
                            }

                            Label {
                                Layout.fillWidth: true
                                text: review.comparison.selectionCompareMode
                                    ? qsTr("Keep one anchor locked while nearby candidates change.")
                                    : qsTr("A local preference event, not a rank or an AI score.")
                                color: review.textMuted
                                font.pixelSize: 10
                            }
                        }

                        ShadowIconButton {
                            source: "qrc:/icons/clear.svg"
                            toolTipText: qsTr("Exit comparison (Esc)")
                            accessibleName: toolTipText
                            enabled: !review.controller.comparisonBusy
                            onClicked: review.comparison.exitComparison()
                        }

                        RowLayout {
                            visible: review.comparison.selectionCompareMode
                            spacing: 5

                            ShadowButton {
                                compact: true
                                text: qsTr("‹ Previous")
                                enabled: !review.controller.comparisonBusy
                                onClicked: review.comparison.navigateCandidate(-1)
                            }

                            ShadowButton {
                                compact: true
                                text: qsTr("Next ›")
                                enabled: !review.controller.comparisonBusy
                                onClicked: review.comparison.navigateCandidate(1)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 12

                        Repeater {
                            model: [review.comparison.leftComparisonSnapshot,
                                    review.comparison.rightComparisonSnapshot]

                            delegate: Rectangle {
                                id: comparisonCard

                                required property int index
                                required property var modelData

                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredWidth: 1
                                radius: 5
                                color: review.panelRaised
                                border.color: review.border

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 8

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Label {
                                            text: review.comparison.selectionCompareMode
                                                ? (comparisonCard.index === 0
                                                    ? qsTr("LOCKED ANCHOR")
                                                    : qsTr("CANDIDATE"))
                                                : (comparisonCard.index === 0
                                                    ? qsTr("LEFT · A") : qsTr("RIGHT · B"))
                                            color: review.accent
                                            font.pixelSize: 9
                                            font.weight: Font.Bold
                                            font.letterSpacing: 1.0
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: comparisonCard.modelData
                                                ? comparisonCard.modelData.title : ""
                                            color: review.textPrimary
                                            horizontalAlignment: Text.AlignRight
                                            elide: Text.ElideMiddle
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Layout.minimumHeight: 180
                                        color: Theme.comparisonCanvas
                                        border.color: Theme.imageBorder

                                        Image {
                                            id: comparisonImage
                                            anchors.fill: parent
                                            anchors.margins: 1
                                            source: comparisonCard.index === 0
                                                ? review.comparison.leftComparisonSource
                                                : review.comparison.rightComparisonSource
                                            fillMode: Image.PreserveAspectFit
                                            asynchronous: true
                                            cache: false
                                            sourceSize.width: 1280
                                            sourceSize.height: 960
                                            onStatusChanged: {
                                                const current = comparisonCard.index === 0
                                                    ? review.comparison.leftComparisonSnapshot
                                                    : review.comparison.rightComparisonSnapshot
                                                if (!review.comparison.sameComparisonIdentity(
                                                        current,
                                                        comparisonCard.modelData)
                                                        || String(comparisonImage.source)
                                                            !== (comparisonCard.index === 0
                                                                ? review.comparison.leftComparisonSource
                                                                : review.comparison.rightComparisonSource))
                                                    return
                                                if (comparisonCard.index === 0)
                                                    review.comparison.leftComparisonVisualReady = status === Image.Ready
                                                else
                                                    review.comparison.rightComparisonVisualReady = status === Image.Ready
                                                Qt.callLater(review.comparison.refreshComparisonReadiness)
                                            }
                                        }

                                        Label {
                                            anchors.centerIn: parent
                                            visible: comparisonImage.status !== Image.Ready
                                            text: comparisonImage.status === Image.Error
                                                ? qsTr("VISUAL LOAD FAILED")
                                                : qsTr("LOADING VERIFIED VISUAL…")
                                            color: comparisonImage.status === Image.Error
                                                ? Theme.errorText : review.textMuted
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Label {
                                            text: comparisonCard.modelData
                                                && comparisonCard.modelData.visualRole.length > 0
                                                ? comparisonCard.modelData.visualRole.toUpperCase()
                                                : qsTr("DISPLAY PROXY")
                                            color: review.textMuted
                                            font.pixelSize: 8
                                            font.weight: Font.Bold
                                            font.letterSpacing: 0.8
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: comparisonCard.modelData
                                                && comparisonCard.modelData.visualWidth > 0
                                                ? qsTr("%L1 × %L2")
                                                    .arg(comparisonCard.modelData.visualWidth)
                                                    .arg(comparisonCard.modelData.visualHeight)
                                                : ""
                                            color: review.textMuted
                                            horizontalAlignment: Text.AlignRight
                                            font.pixelSize: 9
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: review.border
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("TECHNICAL · DISPLAY PROXY")
                                        color: review.textMuted
                                        font.pixelSize: 9
                                        font.weight: Font.DemiBold
                                        font.letterSpacing: 0.8
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && !comparisonCard.modelData.hasTechnicalObservation
                                        text: qsTr("No observation recorded — visual comparison is still available.")
                                        color: Theme.textQuiet
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 9
                                    }

                                    GridLayout {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        columns: 4
                                        rowSpacing: 4
                                        columnSpacing: 8

                                        Label { text: qsTr("MEAN"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.meanLuma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("P50"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.p50Luma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("P01"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.p01Luma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("P99"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.p99Luma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("BLACK"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatPercent(
                                                    comparisonCard.modelData.nearBlackFraction) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("WHITE"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatPercent(
                                                    comparisonCard.modelData.nearWhiteFraction) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("LAPL."); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatProxyDetail(
                                                    comparisonCard.modelData.laplacianVariance) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: qsTr("EDGE"); color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatProxyDetail(
                                                    comparisonCard.modelData.edgeEnergy) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        text: comparisonCard.modelData
                                            ? qsTr("INPUT  %L1 × %L2")
                                                .arg(comparisonCard.modelData.technicalInputWidth)
                                                .arg(comparisonCard.modelData.technicalInputHeight)
                                            : ""
                                        color: Theme.textSubtle
                                        font.pixelSize: 8
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        text: comparisonCard.modelData
                                            ? qsTr("PIPELINE  %1").arg(
                                                review.concisePreprocessingVersion(
                                                    comparisonCard.modelData.technicalPreprocessingVersion))
                                            : ""
                                        color: Theme.textSubtle
                                        elide: Text.ElideRight
                                        font.pixelSize: 8
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        text: comparisonCard.modelData
                                            ? qsTr("ANALYZER  %1").arg(
                                                comparisonCard.modelData.technicalImplementationVersion)
                                            : ""
                                        color: Theme.textSubtle
                                        elide: Text.ElideRight
                                        font.pixelSize: 8
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: review.comparison.comparisonBackendReady
                            ? qsTr("EXACT ARTIFACTS + DECODED %1 FRAMES VERIFIED")
                                .arg("RGBA")
                            : qsTr("WAITING FOR BOTH EXACT COMPARE FRAME RECEIPTS")
                        color: review.comparison.comparisonBackendReady
                            ? Theme.readyText : Theme.textPending
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.7
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Feature models and ranking are not enabled. Technical facts come from each display proxy; different proxy upstreams may not be directly comparable.")
                        color: Theme.warningNoticeText
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: 9
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 78
                        visible: review.comparison.selectionCompareMode
                        spacing: 6

                        Label {
                            text: qsTr("Nearby")
                            color: review.textMuted
                            font.pixelSize: 9
                        }

                        Repeater {
                            model: review.comparison.nearbyCandidateTargets

                            delegate: Rectangle {
                                id: nearbyCandidate

                                required property var modelData

                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.fillHeight: true
                                radius: 4
                                color: Theme.comparisonCanvas
                                border.width: String(modelData.photoId)
                                    === review.selectedPhotoId ? 2 : 1
                                border.color: String(modelData.photoId)
                                    === review.selectedPhotoId
                                    ? review.accent : review.border

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    source: String(nearbyCandidate.modelData.visualSource || "")
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    cache: true
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !review.controller.comparisonBusy
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: review.comparison.chooseCandidate(
                                        nearbyCandidate.modelData)
                                }

                                ToolTip.visible: candidateHover.hovered
                                ToolTip.text: String(
                                    nearbyCandidate.modelData.title || "")

                                HoverHandler { id: candidateHover }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: !review.comparison.selectionCompareMode

                        Repeater {
                            model: ListModel {
                                ListElement { actionId: "left-preferred"; outcomeValue: 0 }
                                ListElement { actionId: "right-preferred"; outcomeValue: 1 }
                                ListElement { actionId: "keep-both"; outcomeValue: 2 }
                                ListElement { actionId: "keep-neither"; outcomeValue: 3 }
                                ListElement { actionId: "cannot-compare"; outcomeValue: 4 }
                            }

                            delegate: ShadowButton {
                                id: outcomeButton

                                required property string actionId
                                required property int outcomeValue

                                Layout.fillWidth: true
                                Layout.preferredHeight: 36
                                variant: ShadowButton.Tinted
                                enabled: review.comparison.canSubmitComparison
                                text: {
                                    switch (outcomeButton.actionId) {
                                    case "left-preferred":
                                        return qsTr("1 · LEFT PREFERRED")
                                    case "right-preferred":
                                        return qsTr("2 · RIGHT PREFERRED")
                                    case "keep-both":
                                        return qsTr("3 · KEEP BOTH")
                                    case "keep-neither":
                                        return qsTr("4 · KEEP NEITHER")
                                    case "cannot-compare":
                                        return qsTr("0 · CANNOT COMPARE")
                                    default:
                                        return ""
                                    }
                                }
                                onClicked: review.comparison.submitComparison(outcomeValue)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: review.comparison.selectionCompareMode

                        ShadowButton {
                            Layout.fillWidth: true
                            text: qsTr("Keep anchor")
                            enabled: review.comparison.canSubmitComparison
                            onClicked: review.comparison.keepAnchor()
                        }

                        ShadowButton {
                            Layout.fillWidth: true
                            variant: ShadowButton.Primary
                            text: qsTr("Promote candidate · Enter")
                            enabled: review.comparison.canSubmitComparison
                            onClicked: review.comparison.promoteCandidate()
                        }

                        ShadowButton {
                            Layout.fillWidth: true
                            variant: ShadowButton.Tinted
                            text: qsTr("Keep both")
                            enabled: review.comparison.canSubmitComparison
                            onClicked: review.comparison.keepBoth()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        BusyIndicator {
                            visible: review.controller.comparisonBusy
                            running: visible
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 22
                        }

                        Label {
                            Layout.fillWidth: true
                            text: review.comparison.localComparisonStatus.length > 0
                                ? review.comparison.localComparisonStatus
                                : review.controller.comparisonStatusText
                            color: review.controller.comparisonBusy
                                ? review.accent : review.textMuted
                            elide: Text.ElideRight
                            font.pixelSize: 9
                        }
                    }
                }
            }
