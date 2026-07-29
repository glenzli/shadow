pragma ComponentBehavior: Bound

import QtQuick

// One visual contract for Like and star-rating evidence across Review cards.
// Placement remains with the owning card; this component keeps icon identity,
// color, density, and optional HUD treatment from drifting between views.
Item {
    id: affinity

    property bool liked: false
    property int rating: 0
    property bool showLike: true
    property bool showRating: true
    property bool floating: false
    property int iconSize: 10
    readonly property int boundedRating: Math.max(0, Math.min(5, rating))
    readonly property int horizontalPadding: floating ? 5 : 0
    readonly property int verticalPadding: floating ? 4 : 0

    visible: (showLike && liked) || (showRating && boundedRating > 0)
    implicitWidth: affinityRow.implicitWidth + horizontalPadding * 2
    implicitHeight: Math.max(iconSize, affinityRow.implicitHeight)
        + verticalPadding * 2

    Rectangle {
        anchors.fill: parent
        visible: affinity.floating
        radius: Theme.compactControlRadius
        color: Theme.previewHudOverlay
        border.width: 1
        border.color: Theme.previewHudBorder
    }

    Row {
        id: affinityRow

        anchors.centerIn: parent
        spacing: 2

        ShadowIcon {
            visible: affinity.showLike && affinity.liked
            source: "qrc:/icons/heart-filled.svg"
            color: Theme.likeAccent
            size: affinity.iconSize
        }

        Repeater {
            model: affinity.showRating ? affinity.boundedRating : 0

            ShadowIcon {
                required property int index

                source: "qrc:/icons/star-filled.svg"
                color: Theme.labelYellow
                size: affinity.iconSize
            }
        }
    }
}
