pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// One-photo review with a horizontal filmstrip. It consumes the same filtered
// model as the grid, so filtering and curation state stay identical across
// both browsing presentations.
Item {
    id: root

    required property var review
    required property var model

    property bool selectingFromFilmstrip: false

    function restoreSelection() {
        if (!visible)
            return
        selectInitialPhoto()
        filmstrip.syncCurrentSelection()
    }

    onVisibleChanged: Qt.callLater(restoreSelection)
    onModelChanged: Qt.callLater(restoreSelection)

    function selectFilmstripPhoto(photo, modifiers) {
        // Keep the pointer target stationary through successive clicks and the
        // second click of a double-click, including partially visible cards.
        selectingFromFilmstrip = true
        try {
            review.selectPhoto(photo, modifiers)
        } finally {
            selectingFromFilmstrip = false
        }
    }

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

    ReviewPreviewViewport {
        id: heroImage
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: filmstripPanel.top
        anchors.margins: 20
        anchors.bottomMargin: 16
        source: root.review.selectedVisualSource
        autoTransform: root.review.selectedVisualAutoTransform
        selectionKey: root.review.selectedPhotoId.length > 0
            ? root.review.selectedPhotoId + "/" + root.review.selectedRepresentationId : ""
    }

    Label {
        anchors.centerIn: heroImage
        visible: root.review.selectedPhotoId.length === 0
        text: qsTr("Select a photo to begin review")
        color: Theme.textMuted
        font.pixelSize: Theme.fontBody
    }

    Rectangle {
        objectName: "singlePhotoMissingSourceBadge"
        anchors.left: heroImage.left
        anchors.bottom: heroImage.bottom
        anchors.leftMargin: 14
        anchors.bottomMargin: 14
        z: 3
        width: missingSourceContents.implicitWidth + 16
        height: 28
        radius: Theme.compactControlRadius
        visible: root.review.selectedPhotoId.length > 0
            && !root.review.selectedSourceAvailable
        color: Theme.warningSurface
        border.width: 1
        border.color: Theme.warningBorder

        Row {
            id: missingSourceContents
            anchors.centerIn: parent
            spacing: 6

            ShadowIcon {
                source: "qrc:/icons/source-missing.svg"
                color: Theme.warningText
                size: 13
            }

            Label {
                text: qsTranslate("ReviewPhotoCard", "ORIGINAL NOT FOUND")
                color: Theme.warningText
                font.pixelSize: Theme.fontMeta
                font.weight: Font.DemiBold
            }
        }
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
            objectName: "reviewFilmstrip"
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
            highlightRangeMode: ListView.NoHighlightRange

            function syncCurrentSelection() {
                if (!root.visible || root.selectingFromFilmstrip)
                    return
                const index = root.model.indexOfPhoto(
                    root.review.selectedPhotoId, root.review.selectedRepresentationId)
                currentIndex = index
                if (index >= 0) {
                    positionViewAtIndex(index, ListView.Contain)
                }
            }

            Component.onCompleted: Qt.callLater(root.restoreSelection)
            onCountChanged: Qt.callLater(root.restoreSelection)
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
                required property string locationId
                required property string visualHandle
                required property string title
                required property string sourcePath
                required property bool sourceAvailable
                required property bool isRemote
                required property bool remoteOriginalCached
                required property string remotePreviewUnavailableReason
                required property string visualRole
                required property string visualError
                required property int visualWidth
                required property int visualHeight
                required property string visualSource
                required property bool visualAutoTransform
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
                required property int imageWidth
                required property int imageHeight
                required property bool hasOrientation
                required property int orientation
                required property bool hasCoordinates
                required property real latitude
                required property real longitude
                required property bool hasAltitude
                required property real altitudeMeters
                required property int metadataSchemaVersion
                required property string localBackingPhotoId
                required property string localBackingRepresentationId
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
                border.width: 0
                clip: true

                ShadowRoundedImage {
                    anchors.fill: parent
                    source: filmCard.visualSource
                    autoTransform: filmCard.visualAutoTransform
                    radius: filmCard.radius
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    smooth: true
                    mipmap: true
                    requestedSourceSize: Qt.size(256, 256)
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 5
                    width: 7
                    height: 7
                    radius: width / 2
                    visible: filmCard.colorLabel !== "none"
                    color: Theme.colorLabel(filmCard.colorLabel)
                }

                ReviewPhotoAffinity {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.rightMargin: 10
                    anchors.topMargin: 9
                    liked: filmCard.liked
                    showRating: false
                    iconSize: 12
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 5
                    anchors.bottomMargin: 5
                    width: 22
                    height: 22
                    radius: Theme.compactControlRadius
                    visible: !filmCard.sourceAvailable
                    color: Theme.warningSurface
                    border.width: 1
                    border.color: Theme.warningBorder

                    ShadowIcon {
                        anchors.centerIn: parent
                        source: "qrc:/icons/source-missing.svg"
                        color: Theme.warningText
                        size: 12
                    }
                }

                ReviewPhotoAffinity {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: 5
                    anchors.bottomMargin: 5
                    liked: false
                    rating: filmCard.decisionRating
                    showLike: false
                    floating: true
                    iconSize: 8
                }

                Rectangle {
                    anchors.fill: parent
                    radius: filmCard.radius
                    color: Theme.transparent
                    border.width: filmCard.selected ? 2 : 0
                    border.color: filmCard.selected
                        ? Theme.accent : Theme.border
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: mouse =>
                        root.selectFilmstripPhoto(filmCard, mouse.modifiers)
                    onDoubleClicked: {
                        root.selectFilmstripPhoto(filmCard, 0)
                        root.review.openSelectedPhoto()
                    }
                }
            }
        }
    }

    Connections {
        target: root.model
        function onModelReset() { Qt.callLater(root.restoreSelection) }
        function onLayoutChanged() { Qt.callLater(root.restoreSelection) }
    }

    Connections {
        target: root.review
        function onSelectedPhotoIdChanged() {
            if (!root.selectingFromFilmstrip)
                Qt.callLater(filmstrip.syncCurrentSelection)
        }
        function onSelectedRepresentationIdChanged() {
            if (!root.selectingFromFilmstrip)
                Qt.callLater(filmstrip.syncCurrentSelection)
        }
    }
}
