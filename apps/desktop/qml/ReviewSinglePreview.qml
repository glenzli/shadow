pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One-photo review with a horizontal filmstrip. It consumes the same filtered
// model as the grid, so filtering and curation state stay identical across
// both browsing presentations.
Item {
    id: root

    required property var review
    required property var model

    function forceGalleryFocus() {
        filmstrip.forceActiveFocus()
    }

    function navigate(direction) {
        const target = review.justifiedReviewLayout.navigationTarget(
            review.selectedPhotoId,
            review.selectedRepresentationId,
            direction,
            0)
        if (!target || String(target.photoId || "").length === 0)
            return
        review.selectPhoto(target, 0)
    }

    function selectInitialPhoto() {
        if (review.selectedPhotoId.length > 0 || filmstrip.count === 0)
            return
        if (filmstrip.currentIndex < 0)
            filmstrip.currentIndex = 0
        if (filmstrip.currentItem === null)
            return
        review.selectPhoto(filmstrip.currentItem, 0)
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.photoCanvas
    }

    Image {
        id: heroImage
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: filmstripPanel.top
        anchors.margins: 20
        anchors.bottomMargin: 16
        source: root.review.selectedVisualSource
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: true
        smooth: true
        mipmap: true
        sourceSize.width: 2048
        sourceSize.height: 2048
    }

    Label {
        anchors.centerIn: heroImage
        visible: root.review.selectedPhotoId.length === 0
        text: qsTr("Select a photo to begin review")
        color: Theme.textMuted
        font.pixelSize: Theme.fontBody
    }

    ReviewDecisionToolbar {
        id: floatingDecisionToolbar
        anchors.right: heroImage.right
        anchors.bottom: filmstripPanel.top
        anchors.rightMargin: 14
        anchors.bottomMargin: 14
        z: 3
        review: root.review
        floating: true
        includeColorLabels: false
        visible: root.review.selectedPhotoId.length > 0
    }

    Rectangle {
        id: filmstripPanel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 112
        color: Theme.chrome
        border.width: 1
        border.color: Theme.border

        ListView {
            id: filmstrip
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            anchors.topMargin: 10
            anchors.bottomMargin: 10
            orientation: ListView.Horizontal
            spacing: 8
            clip: true
            cacheBuffer: 560
            model: root.model
            focus: root.visible
            highlightRangeMode: ListView.ApplyRange
            preferredHighlightBegin: Math.max(0, width * 0.4)
            preferredHighlightEnd: Math.max(0, width * 0.6)

            function syncCurrentSelection() {
                for (let index = 0; index < count; ++index) {
                    const item = itemAtIndex(index)
                    if (item && item.photoId === root.review.selectedPhotoId
                            && item.representationId === root.review.selectedRepresentationId) {
                        currentIndex = index
                        positionViewAtIndex(index, ListView.Contain)
                        return
                    }
                }
            }

            Component.onCompleted: Qt.callLater(root.selectInitialPhoto)
            onCountChanged: Qt.callLater(root.selectInitialPhoto)
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Left)
                    root.navigate(-1)
                else if (event.key === Qt.Key_Right)
                    root.navigate(1)
                else
                    return
                event.accepted = true
            }

            delegate: Rectangle {
                id: filmCard
                required property string photoId
                required property string representationId
                required property string visualHandle
                required property string title
                required property string sourcePath
                required property string visualRole
                required property string visualError
                required property int visualWidth
                required property int visualHeight
                required property string visualSource
                required property bool hasMetadata
                required property string cameraMake
                required property string cameraModel
                required property string lensMake
                required property string lensModel
                required property var capturedAtUnixSeconds
                required property real isoSpeed
                required property real exposureTimeSeconds
                required property real apertureFNumber
                required property real focalLengthMm
                required property real focalLength35mm
                required property int rawWidth
                required property int rawHeight
                required property int sensorBits
                required property string cfaPattern
                required property string dngVersion
                required property var decisionHeadSequence
                required property string decisionFlag
                required property int decisionRating
                required property bool liked
                required property string colorLabel
                required property bool hasDevelopmentEdits
                required property bool hasTechnicalObservation
                required property int technicalInputWidth
                required property int technicalInputHeight
                required property string technicalPreprocessingVersion
                required property string technicalImplementationVersion
                required property real meanLuma
                required property real p01Luma
                required property real p50Luma
                required property real p99Luma
                required property real nearBlackFraction
                required property real nearWhiteFraction
                required property real laplacianVariance
                required property real edgeEnergy

                readonly property bool selected: root.review.isPhotoSelected(
                    photoId, representationId)
                width: Math.max(84, Math.min(126,
                    visualWidth > 0 && visualHeight > 0
                        ? 86 * visualWidth / visualHeight : 108))
                height: filmstrip.height
                radius: Theme.compactControlRadius
                color: Theme.panelRaised
                border.width: selected ? 2 : 1
                border.color: selected ? Theme.accent : Theme.border
                clip: true

                Image {
                    anchors.fill: parent
                    source: filmCard.visualSource
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: true
                    smooth: true
                    mipmap: true
                    sourceSize.width: 256
                    sourceSize.height: 256
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.leftMargin: 5
                    anchors.topMargin: 5
                    width: 7
                    height: 7
                    radius: width / 2
                    visible: filmCard.colorLabel !== "none"
                    color: Theme.colorLabel(filmCard.colorLabel)
                }

                ShadowIcon {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.rightMargin: 5
                    anchors.topMargin: 5
                    visible: filmCard.liked
                    source: "qrc:/icons/heart-filled.svg"
                    color: Theme.likeAccent
                    size: 13
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.review.selectPhoto(filmCard, mouse.modifiers)
                    onDoubleClicked: {
                        root.review.selectPhoto(filmCard, 0)
                        root.review.openSelectedPhoto()
                    }
                }
            }
        }
    }

    Connections {
        target: root.review
        function onSelectedPhotoIdChanged() {
            Qt.callLater(filmstrip.syncCurrentSelection)
        }
        function onSelectedRepresentationIdChanged() {
            Qt.callLater(filmstrip.syncCurrentSelection)
        }
    }
}
