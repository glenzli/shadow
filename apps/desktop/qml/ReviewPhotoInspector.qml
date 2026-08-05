import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Selected-photo inspector index. Identity, EXIF, and comparison transactions
// live in responsibility-named children inside one scrolling surface.
Rectangle {
    id: photoInspector

    required property var review
    required property var metadataPresentation

    signal openMetadataRequested()

    Layout.preferredWidth: 278
    Layout.fillHeight: true
    color: photoInspector.review.panel

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: 1
        color: photoInspector.review.border
    }

    ScrollView {
        id: inspectorScroll

        anchors.fill: parent
        anchors.leftMargin: Theme.panelPadding + 1
        anchors.rightMargin: Theme.panelPadding
        anchors.topMargin: Theme.panelPadding
        anchors.bottomMargin: Theme.panelPadding
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: inspectorScroll.availableWidth
            spacing: 10

            ReviewPhotoSummary {
                Layout.fillWidth: true
                review: photoInspector.review
            }

            ReviewSelectionInspectionPane {
                Layout.fillWidth: true
                review: photoInspector.review
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                visible: photoInspector.review.galleryPresentation
                    === ReviewWorkspace.SinglePhotoFilmstrip
                    && photoInspector.review.selectedPhotoId.length > 0
                color: photoInspector.review.border
            }

            ReviewExifSection {
                Layout.fillWidth: true
                review: photoInspector.review
                metadataPresentation: photoInspector.metadataPresentation
                onOpenMetadataRequested:
                    photoInspector.openMetadataRequested()
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                visible: photoInspector.review.selectedPhotoId.length > 0
                    && photoInspector.review.galleryPresentation
                        === ReviewWorkspace.SinglePhotoFilmstrip
                color: photoInspector.review.border
            }

            ReviewComparisonSlots {
                Layout.fillWidth: true
                visible: photoInspector.review.galleryPresentation
                    === ReviewWorkspace.SinglePhotoFilmstrip
                review: photoInspector.review
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: photoInspector.review.border
            }

            Label {
                Layout.fillWidth: true
                visible: photoInspector.review.precisionOpenStatus.length > 0
                text: photoInspector.review.precisionOpenStatus
                color: Theme.warningText
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontMeta
            }
        }
    }
}
