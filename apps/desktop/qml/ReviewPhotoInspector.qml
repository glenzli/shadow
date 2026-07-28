pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The selected-photo information surface is independent from gallery layout
// and comparison rendering; it consumes only the workspace selection contract.
Rectangle {
    id: photoInspector

    required property var review
    required property var metadataPresentation

    signal openMetadataRequested()

    function localizedVisualRole(role) {
        switch (String(role).trim().toLowerCase()) {
        case "recipe":
            return qsTr("RECIPE")
        case "proxy":
            return qsTr("PROXY")
        default:
            return String(role).toUpperCase()
        }
    }

            Layout.preferredWidth: 278
            Layout.fillHeight: true
            color: review.panel

            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: 1
                color: review.border
            }

            ScrollView {
                id: photoInspectorScroll

                anchors.fill: parent
                anchors.leftMargin: Theme.panelPadding + 1
                anchors.rightMargin: Theme.panelPadding
                anchors.topMargin: Theme.panelPadding
                anchors.bottomMargin: Theme.panelPadding
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: photoInspectorScroll.availableWidth
                    spacing: 10

                Label {
                    text: qsTr("PHOTO")
                    color: review.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.6
                }

                Label {
                    Layout.fillWidth: true
                    text: review.selectedTitle.length > 0
                        ? review.selectedTitle : qsTr("Nothing selected")
                    color: review.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: review.selectedPath
                    color: review.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.NoWrap
                    maximumLineCount: 1
                    elide: Text.ElideMiddle
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: review.border
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: review.selectedRole.length > 0
                            ? photoInspector.localizedVisualRole(review.selectedRole)
                            : qsTr("PENDING")
                        color: review.textPrimary
                        font.pixelSize: 10
                        font.weight: Font.Medium
                    }

                    Rectangle {
                        Layout.preferredWidth: 3
                        Layout.preferredHeight: 3
                        radius: 1.5
                        color: review.textMuted
                    }

                    Label {
                        Layout.fillWidth: true
                        text: review.selectedWidth > 0
                            ? qsTr("%L1 × %L2").arg(review.selectedWidth)
                                .arg(review.selectedHeight)
                            : "—"
                        color: review.textMuted
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: review.border
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    visible: review.selectedPhotoId.length > 0
                    color: review.border
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: review.selectedPhotoId.length > 0
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            text: qsTr("EXIF")
                            color: review.textMuted
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.2
                        }

                        Item { Layout.fillWidth: true }

                        ShadowIconButton {
                            source: "qrc:/icons/metadata.svg"
                            iconSize: 15
                            toolTipText: qsTr("View all photo metadata")
                            accessibleName: toolTipText
                            enabled: review.selectedPhotoId.length > 0
                            onClicked: photoInspector.openMetadataRequested()
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: !review.selectedHasMetadata
                        text: review.controller.photoInspectionFailed
                            ? qsTr("Could not load metadata for this photo")
                            : review.controller.photoInspectionBusy
                                ? qsTr("Metadata is being prepared")
                                : qsTr("No metadata is available for this photo")
                        color: Theme.textQuiet
                        font.pixelSize: 10
                    }

                    ShadowButton {
                        visible: !review.selectedHasMetadata
                            && review.controller.photoInspectionFailed
                        text: qsTr("Retry")
                        variant: ShadowButton.Ghost
                        enabled: !review.controller.photoInspectionBusy
                        onClicked: review.controller.retryPhotoInspection()
                    }

                    Repeater {
                        model: [
                            { id: "captured_at", label: qsTr("CAPTURED") },
                            { id: "camera", label: qsTr("CAMERA") },
                            { id: "lens", label: qsTr("LENS") },
                            { id: "exposure", label: qsTr("SHUTTER") },
                            { id: "aperture", label: qsTr("APERTURE") },
                            { id: "iso", label: qsTr("SENSITIVITY") },
                            { id: "focal_length", label: qsTr("FOCAL LENGTH") },
                            { id: "dimensions", label: qsTr("PREVIEW") },
                            { id: "focal_length_35mm", label: qsTr("35 MM EQUIV.") },
                            { id: "raw_dimensions", label: qsTr("RAW SIZE") },
                            { id: "sensor_bits", label: qsTr("BIT DEPTH") },
                            { id: "cfa", label: qsTr("CFA") },
                            { id: "dng", label: qsTr("DNG") }
                        ]

                        delegate: RowLayout {
                            id: exifRow
                            required property var modelData
                            Layout.fillWidth: true
                            visible: review.selectedHasMetadata
                                && review.preferences.exifFields.indexOf(modelData.id) >= 0
                            spacing: 8

                            Label {
                                Layout.preferredWidth: 76
                                horizontalAlignment: Text.AlignRight
                                text: exifRow.modelData.label
                                color: review.textMuted
                                font.pixelSize: 9
                            }

                            Label {
                                Layout.fillWidth: true
                                text: photoInspector.metadataPresentation.exifValue(
                                    exifRow.modelData.id)
                                color: review.textPrimary
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    visible: review.selectedPhotoId.length > 0
                    color: review.border
                }

                ColumnLayout {
                    id: compareSection

                    Layout.fillWidth: true
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("COMPARE SLOTS")
                            color: review.textMuted
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                        }

                        ShadowIconButton {
                            id: compareButton
                            source: "qrc:/icons/compare.svg"
                            iconSize: 17
                            variant: ShadowIconButton.Tinted
                            toolTipText: qsTr("Compare slots A and B")
                            accessibleName: toolTipText
                            enabled: review.comparison.comparisonReady
                                && !review.comparison.compareMode
                                && !review.controller.comparisonBusy
                                && !review.controller.decisionBusy
                                && !review.controller.scanning
                                && !review.controller.refreshing
                                && !review.controller.busy
                                && !review.controller.loadingMore
                            onClicked: review.comparison.enterComparison()
                        }

                        ShadowIconButton {
                            source: "qrc:/icons/clear.svg"
                            iconSize: 16
                            toolTipText: qsTr("Clear comparison slots")
                            accessibleName: toolTipText
                            enabled: !review.controller.comparisonBusy
                                && !review.controller.decisionBusy
                                && !review.controller.scanning
                                && !review.controller.refreshing
                                && !review.controller.busy
                                && !review.controller.loadingMore
                                && (review.comparison.leftComparisonSnapshot !== null
                                    || review.comparison.rightComparisonSnapshot !== null)
                            onClicked: review.comparison.clearComparisonSlots()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        radius: 6
                        color: Theme.surfaceSubtle

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 3
                            spacing: 6

                            Label {
                                Layout.fillWidth: true
                                text: review.comparison.leftComparisonSnapshot
                                    ? qsTr("A  %1").arg(
                                        review.comparison.leftComparisonSnapshot.title)
                                    : qsTr("A  Not set")
                                color: review.comparison.leftComparisonSnapshot
                                    ? review.textPrimary : review.textMuted
                                elide: Text.ElideMiddle
                                font.pixelSize: 10
                                font.weight: Font.Medium
                            }

                            ShadowIconButton {
                                id: setLeftButton
                                buttonSize: 28
                                source: "qrc:/icons/slot-left.svg"
                                toolTipText: qsTr("Set selected photo as comparison slot A")
                                accessibleName: toolTipText
                                enabled: !review.comparison.compareMode
                                    && !review.controller.comparisonBusy
                                    && !review.controller.decisionBusy
                                    && !review.controller.scanning
                                    && !review.controller.refreshing
                                    && !review.controller.busy
                                    && !review.controller.loadingMore
                                    && review.selectedPhotoId.length > 0
                                    && review.selectedRepresentationId.length > 0
                                    && review.selectedVisualHandle.length > 0
                                    && review.selectedVisualSource.length > 0
                                    && (review.comparison.rightComparisonSnapshot === null
                                        || review.comparison.rightComparisonSnapshot.photoId
                                            !== review.selectedPhotoId)
                                onClicked: review.comparison.setSelectedAsLeft()
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        radius: 6
                        color: Theme.surfaceSubtle

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 3
                            spacing: 6

                            Label {
                                Layout.fillWidth: true
                                text: review.comparison.rightComparisonSnapshot
                                    ? qsTr("B  %1").arg(
                                        review.comparison.rightComparisonSnapshot.title)
                                    : qsTr("B  Not set")
                                color: review.comparison.rightComparisonSnapshot
                                    ? review.textPrimary : review.textMuted
                                elide: Text.ElideMiddle
                                font.pixelSize: 10
                                font.weight: Font.Medium
                            }

                            ShadowIconButton {
                                id: setRightButton
                                buttonSize: 28
                                source: "qrc:/icons/slot-right.svg"
                                toolTipText: qsTr("Set selected photo as comparison slot B")
                                accessibleName: toolTipText
                                enabled: !review.comparison.compareMode
                                    && !review.controller.comparisonBusy
                                    && !review.controller.decisionBusy
                                    && !review.controller.scanning
                                    && !review.controller.refreshing
                                    && !review.controller.busy
                                    && !review.controller.loadingMore
                                    && review.selectedPhotoId.length > 0
                                    && review.selectedRepresentationId.length > 0
                                    && review.selectedVisualHandle.length > 0
                                    && review.selectedVisualSource.length > 0
                                    && (review.comparison.leftComparisonSnapshot === null
                                        || review.comparison.leftComparisonSnapshot.photoId
                                            !== review.selectedPhotoId)
                                onClicked: review.comparison.setSelectedAsRight()
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: review.selectedPhotoId.length > 0
                            && review.selectedVisualSource.length === 0
                        text: qsTr("A display visual is required for comparison.")
                        color: Theme.warningNoticeText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 10
                    }

                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: review.border
                }

                Label {
                    Layout.fillWidth: true
                    visible: review.precisionOpenStatus.length > 0
                    text: review.precisionOpenStatus
                    color: Theme.warningText
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontMeta
                }

                }
            }
        }
