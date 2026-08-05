pragma ComponentBehavior: Bound
pragma Translator: "ReviewWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Lightweight culling evidence for the primary selection. The proxy crop is
// immediate; the exact level-zero region replaces it without blocking Review.
ColumnLayout {
    id: pane

    required property var review

    readonly property real focusX: Math.max(0, Math.min(
        1, pane.review.selectedFocusObservationCenterX))
    readonly property real focusY: Math.max(0, Math.min(
        1, pane.review.selectedFocusObservationCenterY))

    function percent(value) {
        return (Number(value) * 100).toLocaleString(
            Qt.locale(), "f", Number(value) < 0.01 ? 2 : 1) + "%"
    }

    visible: pane.review.galleryPresentation
        === ReviewWorkspace.SinglePhotoFilmstrip
        && pane.review.selectedPhotoId.length > 0
    spacing: 8

    RowLayout {
        Layout.fillWidth: true

        Label {
            Layout.fillWidth: true
            text: qsTr("FOCUS CHECK")
            color: pane.review.textMuted
            font.pixelSize: 10
            font.weight: Font.DemiBold
        }

        Label {
            text: pane.review.controller.focusDetailReady
                ? qsTr("100%") : qsTr("PROXY")
            color: pane.review.controller.focusDetailReady
                ? Theme.readyText : pane.review.textMuted
            font.pixelSize: 9
            font.weight: Font.DemiBold
        }
    }

    Rectangle {
        id: detailFrame

        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(width, 238)
        radius: 7
        color: Theme.photoCanvas
        border.width: 1
        border.color: pane.review.border
        clip: true

        Image {
            id: proxyDetail

            visible: pane.review.selectedHasFocusObservation
                && !pane.review.controller.focusDetailReady
            asynchronous: true
            cache: true
            source: pane.review.selectedVisualSource
            fillMode: Image.PreserveAspectFit
            readonly property real proxyScale: Math.max(
                1, detailFrame.width / Math.max(1, pane.review.selectedWidth),
                detailFrame.height / Math.max(1, pane.review.selectedHeight))
            width: Math.max(1, pane.review.selectedWidth) * proxyScale
            height: Math.max(1, pane.review.selectedHeight) * proxyScale
            x: detailFrame.width / 2 - pane.focusX * width
            y: detailFrame.height / 2 - pane.focusY * height
        }

        Image {
            id: exactDetail

            visible: pane.review.controller.focusDetailReady
            asynchronous: true
            cache: false
            source: pane.review.controller.focusDetailImageSource
            width: implicitWidth
            height: implicitHeight
            x: Math.round((detailFrame.width - width) / 2)
            y: Math.round((detailFrame.height - height) / 2)
        }

        Rectangle {
            anchors.centerIn: parent
            visible: pane.review.selectedHasFocusObservation
            width: 20
            height: 20
            radius: 2
            color: "transparent"
            border.width: 1
            border.color: Theme.focusRing
        }

        Label {
            anchors.centerIn: parent
            width: parent.width - 28
            visible: !pane.review.selectedHasFocusObservation
            text: qsTr("No camera focus record")
            color: pane.review.textMuted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pixelSize: 10
        }

        Rectangle {
            anchors.centerIn: parent
            visible: pane.review.selectedHasFocusObservation
            width: 4
            height: 4
            radius: 2
            color: Theme.focusRing
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 28
            visible: pane.review.selectedHasFocusObservation
            color: Theme.previewHudStrongOverlay

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 6

                BusyIndicator {
                    visible: pane.review.controller.focusDetailBusy
                    running: visible
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                }

                Label {
                    Layout.fillWidth: true
                    text: pane.review.controller.focusDetailStatusText
                    color: Theme.accentForeground
                    elide: Text.ElideRight
                    font.pixelSize: 9
                }
            }
        }
    }

    GridLayout {
        Layout.fillWidth: true
        visible: pane.review.selectedHasTechnicalObservation
        columns: 2
        columnSpacing: 6
        rowSpacing: 5

        Label {
            text: qsTr("Shadow reference")
            color: pane.review.textMuted
            font.pixelSize: 9
        }
        Label {
            Layout.alignment: Qt.AlignRight
            text: pane.percent(pane.review.selectedNearBlackFraction)
            color: pane.review.selectedNearBlackFraction >= 0.03
                ? Theme.warningText : pane.review.textPrimary
            font.pixelSize: 10
        }
        Label {
            text: qsTr("Highlight reference")
            color: pane.review.textMuted
            font.pixelSize: 9
        }
        Label {
            Layout.alignment: Qt.AlignRight
            text: pane.percent(pane.review.selectedNearWhiteFraction)
            color: pane.review.selectedNearWhiteFraction >= 0.01
                ? Theme.warningText : pane.review.textPrimary
            font.pixelSize: 10
        }
        Label {
            text: qsTr("Proxy detail")
            color: pane.review.textMuted
            font.pixelSize: 9
        }
        Label {
            Layout.alignment: Qt.AlignRight
            text: pane.review.selectedLaplacianVariance.toLocaleString(
                Qt.locale(), "f", 3)
            color: pane.review.textPrimary
            font.pixelSize: 10
        }
    }

}
