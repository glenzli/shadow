pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The decision controls are deliberately shared by the gallery and the
// single-photo filmstrip view. A Like therefore remains in the same curation
// muscle-memory group as pick/reject, rating, and color labels.
Item {
    id: root

    required property var review
    property bool floating: false
    property bool includeColorLabels: true

    readonly property bool decisionEnabled: review.canMutateDecision
        && review.selectedPhotoCount === 1
    readonly property int controlSize: 28

    implicitWidth: controls.implicitWidth + horizontalInset * 2
    implicitHeight: controls.implicitHeight + verticalInset * 2
    readonly property int horizontalInset: floating ? 8 : 0
    readonly property int verticalInset: floating ? 6 : 0

    Rectangle {
        anchors.fill: parent
        visible: root.floating
        radius: Theme.controlRadius + 2
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    RowLayout {
        id: controls
        anchors.centerIn: parent
        spacing: 2

        ShadowIconButton {
            buttonSize: root.controlSize
            iconSize: 15
            source: "qrc:/icons/pick.svg"
            selected: root.review.selectedDecisionFlag === "picked"
            selectedSurfaceColor: Theme.successSurface
            selectedHoverSurfaceColor: Theme.successSurface
            selectedPressedSurfaceColor: Theme.successSurface
            selectedIconColor: Theme.successText
            toolTipText: root.review.selectedDecisionFlag === "picked"
                ? qsTr("Clear pick flag") : qsTr("Pick selected photo")
            accessibleName: toolTipText
            enabled: root.decisionEnabled
            onClicked: root.review.setSelectedFlag(
                root.review.selectedDecisionFlag === "picked" ? "unflagged" : "picked")
        }

        ShadowIconButton {
            buttonSize: root.controlSize
            iconSize: 15
            source: "qrc:/icons/reject.svg"
            variant: ShadowIconButton.Ghost
            selected: root.review.selectedDecisionFlag === "rejected"
            selectedSurfaceColor: Theme.dangerSurface
            selectedHoverSurfaceColor: Theme.dangerHoverSurface
            selectedPressedSurfaceColor: Theme.dangerPressedSurface
            selectedIconColor: Theme.dangerText
            toolTipText: root.review.selectedDecisionFlag === "rejected"
                ? qsTr("Clear reject flag") : qsTr("Reject selected photo")
            accessibleName: toolTipText
            enabled: root.decisionEnabled
            onClicked: root.review.setSelectedFlag(
                root.review.selectedDecisionFlag === "rejected" ? "unflagged" : "rejected")
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 17
            color: Theme.border
        }

        Repeater {
            model: 5

            delegate: ShadowIconButton {
                required property int index
                buttonSize: root.controlSize - 2
                iconSize: 14
                source: root.review.selectedDecisionRating > index
                    ? "qrc:/icons/star-filled.svg" : "qrc:/icons/star.svg"
                selected: root.review.selectedDecisionRating === index + 1
                selectedSurfaceColor: Theme.warningSurface
                selectedHoverSurfaceColor: Theme.warningSurface
                selectedPressedSurfaceColor: Theme.warningSurface
                selectedIconColor: Theme.warningText
                foregroundColor: root.review.selectedDecisionRating > index
                    ? Theme.warningText : Theme.textMuted
                toolTipText: qsTr("Rate %1 stars").arg(index + 1)
                accessibleName: toolTipText
                enabled: root.decisionEnabled
                onClicked: root.review.setSelectedRating(index + 1)
            }
        }

        ShadowIconButton {
            buttonSize: root.controlSize
            iconSize: 16
            source: root.review.selectedLiked
                ? "qrc:/icons/heart-filled.svg" : "qrc:/icons/heart.svg"
            selected: root.review.selectedLiked
            selectedSurfaceColor: Theme.likeSurface
            selectedHoverSurfaceColor: Theme.likeHoverSurface
            selectedPressedSurfaceColor: Theme.likePressedSurface
            selectedIconColor: Theme.likeAccent
            foregroundColor: Theme.likeAccent
            toolTipText: root.review.selectedLiked
                ? qsTr("Remove Like from selected photo")
                : qsTr("Like selected photo")
            accessibleName: toolTipText
            enabled: root.decisionEnabled
            onClicked: root.review.controller.setPhotoLiked(
                root.review.selectedPhotoId, !root.review.selectedLiked)
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 17
            color: Theme.border
        }

        ShadowIconButton {
            buttonSize: root.controlSize
            iconSize: 16
            source: "qrc:/icons/compare.svg"
            selected: root.review.culling.containsCandidate(
                root.review.selectedVisualSnapshot())
            selectedSurfaceColor: Theme.accentSurface
            selectedHoverSurfaceColor: Theme.accentSurface
            selectedPressedSurfaceColor: Theme.accentSurfacePressed
            selectedIconColor: Theme.accentSelectionText
            toolTipText: selected
                ? qsTr("Remove selected photo from candidates")
                : qsTr("Add selected photo to candidates (C)")
            accessibleName: toolTipText
            enabled: root.review.selectedPhotoCount === 1
                && root.review.selectedVisualSource.length > 0
                && !root.review.comparison.compareMode
                && !root.review.culling.arenaActive
            onClicked: root.review.toggleSelectedCandidate()
        }

        Rectangle {
            visible: root.includeColorLabels
            Layout.preferredWidth: visible ? 1 : 0
            Layout.preferredHeight: 17
            color: Theme.border
        }

        Repeater {
            model: root.includeColorLabels
                ? ["red", "yellow", "green", "blue", "purple"] : []

            delegate: ShadowColorLabelButton {
                required property string modelData
                labelColor: Theme.colorLabel(modelData)
                selected: root.review.selectedColorLabel === modelData
                toolTipText: qsTr("Set color label: %1").arg(modelData)
                accessibleName: toolTipText
                enabled: root.decisionEnabled
                onClicked: root.review.controller.setPhotoColorLabel(
                    root.review.selectedPhotoId,
                    root.review.selectedColorLabel === modelData ? "none" : modelData)
            }
        }
    }
}
