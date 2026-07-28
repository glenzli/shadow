pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects

Item {
    id: card

    required property var workspace
    required property var entry
    readonly property int captionHeight: 48
    readonly property int surfaceRadius: 9

    readonly property string photoId: String(entry.photoId || "")
    readonly property string representationId: String(entry.representationId || "")
    readonly property string visualHandle: String(entry.visualHandle || "")
    readonly property var decisionHeadSequence: entry.decisionHeadSequence || 0
    readonly property string decisionFlag: String(entry.decisionFlag || "unflagged")
    readonly property int decisionRating: Number(entry.decisionRating || 0)
    readonly property bool liked: Boolean(entry.liked)
    readonly property string colorLabel: String(entry.colorLabel || "none")
    readonly property bool hasDevelopmentEdits: Boolean(entry.hasDevelopmentEdits)
    readonly property string title: String(entry.title || "")
    readonly property string sourcePath: String(entry.sourcePath || "")
    readonly property string visualRole: String(entry.visualRole || "")
    readonly property string visualError: String(entry.visualError || "")
    readonly property int visualWidth: Number(entry.visualWidth || 0)
    readonly property int visualHeight: Number(entry.visualHeight || 0)
    readonly property string visualSource: String(entry.visualSource || "")
    readonly property bool hasMetadata: Boolean(entry.hasMetadata)
    readonly property string cameraMake: String(entry.cameraMake || "")
    readonly property string cameraModel: String(entry.cameraModel || "")
    readonly property string lensMake: String(entry.lensMake || "")
    readonly property string lensModel: String(entry.lensModel || "")
    readonly property var capturedAtUnixSeconds: entry.capturedAtUnixSeconds || 0
    readonly property real isoSpeed: Number(entry.isoSpeed || 0)
    readonly property real exposureTimeSeconds: Number(entry.exposureTimeSeconds || 0)
    readonly property real apertureFNumber: Number(entry.apertureFNumber || 0)
    readonly property real focalLengthMm: Number(entry.focalLengthMm || 0)
    readonly property real focalLength35mm: Number(entry.focalLength35mm || 0)
    readonly property int rawWidth: Number(entry.rawWidth || 0)
    readonly property int rawHeight: Number(entry.rawHeight || 0)
    readonly property int sensorBits: Number(entry.sensorBits || 0)
    readonly property string cfaPattern: String(entry.cfaPattern || "")
    readonly property string dngVersion: String(entry.dngVersion || "")
    readonly property bool hasTechnicalObservation: Boolean(entry.hasTechnicalObservation)
    readonly property int technicalInputWidth: Number(entry.technicalInputWidth || 0)
    readonly property int technicalInputHeight: Number(entry.technicalInputHeight || 0)
    readonly property string technicalPreprocessingVersion: String(entry.technicalPreprocessingVersion || "")
    readonly property string technicalImplementationVersion: String(entry.technicalImplementationVersion || "")
    readonly property real meanLuma: Number(entry.meanLuma || 0)
    readonly property real p01Luma: Number(entry.p01Luma || 0)
    readonly property real p50Luma: Number(entry.p50Luma || 0)
    readonly property real p99Luma: Number(entry.p99Luma || 0)
    readonly property real nearBlackFraction: Number(entry.nearBlackFraction || 0)
    readonly property real nearWhiteFraction: Number(entry.nearWhiteFraction || 0)
    readonly property real laplacianVariance: Number(entry.laplacianVariance || 0)
    readonly property real edgeEnergy: Number(entry.edgeEnergy || 0)
    readonly property bool selected:
        workspace.isPhotoSelected(photoId, representationId)

    Component.onDestruction: cardMenu.releaseOwner(card)

    Accessible.role: Accessible.ListItem
    Accessible.name: title
    Accessible.selected: selected

    RectangularShadow {
        x: 5
        y: 5
        width: parent.width - 10
        height: parent.height - 10
        offset: Qt.vector2d(0, 2)
        radius: card.surfaceRadius
        blur: 12
        spread: -2
        color: Theme.shadowSoft
        opacity: card.selected ? 0.9 : cardMouse.containsMouse ? 0.35 : 0.0
        cached: true

        Behavior on opacity {
            NumberAnimation { duration: 120 }
        }
    }

    Rectangle {
        id: cardSurface
        anchors.fill: parent
        anchors.margins: 0
        radius: card.surfaceRadius
        clip: true
        color: Theme.panelRaised
        border.width: 1
        border.color: card.selected ? Theme.accentBorder : Theme.border
        scale: cardMouse.pressed ? 0.995 : 1.0

        Behavior on scale {
            NumberAnimation { duration: 80 }
        }

        Image {
            id: thumbnail
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: caption.top
            source: card.visualSource
            // The layout width itself is derived from visualWidth/visualHeight.
            // Fitting is a defensive guarantee for imperfect/late metadata, never
            // a photo crop mode.
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: true
            // Fine foliage and fabric otherwise alias when a warm 1024px
            // gallery texture is displayed as a much smaller card. Keep
            // texture filtering stable while the SCALE control changes layout.
            smooth: true
            mipmap: true
            // Gallery SCALE only changes layout geometry. It must not create a
            // new image-provider request for every card while the thumb is
            // dragged: that defeats both Qt's image cache and Shadow's local
            // proxy cache. A fixed 1024px presentation layer remains sharp at
            // the allowed gallery scale and reuses one decoded cache entry.
            sourceSize.width: 1024
            sourceSize.height: 1024
        }

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.topMargin: 12
            anchors.leftMargin: 12
            width: 9
            height: 9
            radius: width / 2
            visible: card.colorLabel !== "none"
            color: Theme.colorLabel(card.colorLabel)
        }

        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 10
            anchors.rightMargin: 10
            width: 24
            height: 24
            radius: Theme.compactControlRadius
            visible: card.decisionFlag !== "unflagged"
            color: card.decisionFlag === "picked"
                ? Theme.successSurface : Theme.dangerSurface
            border.color: card.decisionFlag === "picked"
                ? Theme.successBorder : Theme.dangerBorder

            ShadowIcon {
                anchors.centerIn: parent
                source: card.decisionFlag === "picked"
                    ? "qrc:/icons/pick.svg" : "qrc:/icons/reject.svg"
                color: card.decisionFlag === "picked"
                    ? Theme.successText : Theme.dangerText
                size: 14
            }
        }

        Rectangle {
            anchors.fill: thumbnail
            visible: card.visualSource.length === 0
            color: Theme.surfaceSubtle

            Column {
                anchors.centerIn: parent
                spacing: 8
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "RAW"
                    color: Theme.rawPlaceholderText
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                    font.letterSpacing: 2
                }
                Label {
                    text: card.visualError.length > 0
                        ? qsTr("PREVIEW PENDING") : qsTr("NO VISUAL")
                    color: Theme.textMuted
                    font.pixelSize: 9
                }
            }
        }

        Rectangle {
            id: caption
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: card.captionHeight
            color: card.selected
                ? Theme.accentSelectionSurface : Theme.thumbnailCaptionOverlay

            Rectangle {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: 3
                height: 24
                radius: 1.5
                visible: card.selected
                color: Theme.accent
            }

            Column {
                anchors.left: parent.left
                anchors.right: cardMetadata.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 9
                spacing: 2
                Label {
                    width: parent.width
                    text: card.title
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                    font.pixelSize: 11
                }
                Label {
                    text: card.visualWidth > 0
                        ? qsTr("%L1 × %L2").arg(card.visualWidth)
                            .arg(card.visualHeight) : qsTr("awaiting cache")
                    color: Theme.textMuted
                    font.pixelSize: 9
                }
            }

            Column {
                id: cardMetadata
                anchors.right: parent.right
                anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                ShadowIcon {
                    anchors.right: parent.right
                    visible: card.hasDevelopmentEdits
                    source: "qrc:/icons/edit.svg"
                    color: Theme.accent
                    size: 12
                }

                Row {
                    anchors.right: parent.right
                    visible: card.decisionRating > 0
                    spacing: 1
                    Repeater {
                        model: card.decisionRating
                        ShadowIcon {
                            required property int index
                            source: "qrc:/icons/star-filled.svg"
                            color: Theme.accent
                            size: 8
                        }
                    }
                }
            }
        }

        ReviewPhotoContextMenu {
            id: cardMenu
        }

        MouseArea {
            id: cardMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: mouse => {
                const preserveSelection = mouse.button === Qt.RightButton
                    && card.selected
                if (!preserveSelection)
                    workspace.selectPhoto(card, mouse.modifiers)
                if (mouse.button === Qt.RightButton) {
                    workspace.controller.refreshSharedGradeNodes()
                    cardMenu.openAt(
                        cardMouse, mouse.x, mouse.y, card.workspace,
                        card.photoId, card.liked, card.decisionFlag)
                }
            }
            onDoubleClicked: mouse => {
                if (mouse.button !== Qt.LeftButton)
                    return
                workspace.selectPhoto(card, 0)
                workspace.openSelectedPhoto()
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 0
        z: 2
        visible: card.selected
        radius: card.surfaceRadius
        color: Theme.transparent
        border.width: 3
        border.color: Theme.accent
    }
}
