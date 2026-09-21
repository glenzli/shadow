pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: bar
    required property var workspace
    signal backRequested()
    color: Theme.panelRaised
    border.color: Theme.border
    implicitHeight: 44

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 12
        ShadowButton {
            text: qsTr("People")
            variant: ShadowButton.Ghost
            onClicked: bar.backRequested()
        }
        Label {
            Layout.fillWidth: true
            text: String(bar.workspace.controller.personFilterName || "")
            font.pixelSize: Theme.fontSection
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            elide: Text.ElideRight
        }
        ShadowButton {
            objectName: "peopleSplitPhotosButton"
            text: qsTr("Separate selected photos")
            variant: ShadowButton.Ghost
            enabled: bar.workspace.peopleController
                && !bar.workspace.peopleController.busy
                && bar.workspace.selectedPhotoCount > 0
                && bar.workspace.selectedPhotoCount < bar.workspace.controller.personPhotoCount
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Move these photos to a separate person. You can undo this correction.")
            onClicked: {
                const photos = bar.workspace.batchSelectionTargets().map(target => String(target.photoId))
                bar.workspace.peopleController.splitGroupPhotos(bar.workspace.controller.personFilterId, photos)
            }
        }
        ShadowButton {
            text: qsTr("Undo correction")
            variant: ShadowButton.Ghost
            enabled: bar.workspace.peopleController
                && !bar.workspace.peopleController.busy
                && bar.workspace.peopleController.canUndoMerge
            onClicked: bar.workspace.peopleController.undoLastMerge()
        }
    }
}
