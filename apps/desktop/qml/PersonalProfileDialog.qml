pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Popup {
    id: root
    objectName: "personalProfileDialog"

    required property var profile
    required property var controller
    required property var locationSearch
    required property real hostWidth
    required property real hostHeight
    property string nicknameDraft: ""

    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(620, Math.max(0, hostWidth - 48))
    height: Math.min(560, Math.max(0, hostHeight - 48))
    x: Math.round(((parent ? parent.width : hostWidth) - width) / 2)
    y: Math.round(((parent ? parent.height : hostHeight) - height) / 2)
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function synchronizeDrafts() {
        nicknameDraft = String(profile.nickname)
        const copied = []
        for (let index = 0; index < profile.livingPlaces.length; ++index)
            copied.push(livingPlacesEditor.copyPlace(profile.livingPlaces[index]))
        livingPlacesEditor.places = copied
    }

    function present() {
        synchronizeDrafts()
        livingPlacesEditor.resetSearch()
        controller.refreshTravelCollections()
        open()
    }

    function saveAndClose() {
        profile.nickname = nicknameDraft
        if (profile.replaceLivingPlaces(livingPlacesEditor.places)
                && String(profile.errorText).length === 0)
            close()
    }

    Connections {
        target: root.controller
        function onTravelCollectionsChanged() {
            livingPlacesEditor.synchronizeLibraryIndex()
        }
    }

    FileDialog {
        id: avatarFileDialog
        title: qsTr("Choose an avatar")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.heic *.webp)")]
        onAccepted: root.profile.importAvatar(selectedFile)
    }

    background: Rectangle {
        radius: 12
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 58
            color: Theme.chrome
            radius: 12

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.border
            }

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.right: doneButton.left
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1

                Label {
                    width: parent.width
                    text: qsTr("Personal profile")
                    color: Theme.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Label {
                    width: parent.width
                    text: qsTr("Private context stored only on this device")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                }
            }

            ShadowButton {
                id: doneButton
                objectName: "personalProfileDoneButton"
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                variant: ShadowButton.Primary
                text: qsTr("Done")
                onClicked: root.saveAndClose()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            ColumnLayout {
                x: 24
                width: Math.max(0, root.width - 48)
                spacing: 18

                Item { Layout.preferredHeight: 4 }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 18

                    Item {
                        Layout.preferredWidth: 88
                        Layout.preferredHeight: 88

                        Rectangle {
                            anchors.fill: parent
                            radius: width / 2
                            color: Theme.panel
                            border.width: 1
                            border.color: Theme.borderStrong
                        }

                        ShadowRoundedImage {
                            anchors.fill: parent
                            anchors.margins: 3
                            visible: String(root.profile.avatarUrl).length > 0
                            source: root.profile.avatarUrl
                            radius: width / 2
                            fillMode: Image.PreserveAspectCrop
                            requestedSourceSize: Qt.size(176, 176)
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: String(root.profile.avatarUrl).length === 0
                            text: root.profile.avatarInitial
                            color: Theme.textPrimary
                            font.pixelSize: 30
                            font.weight: Font.DemiBold
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            text: qsTr("Avatar")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Shown in Shadow's title bar. The image never leaves this device.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.Wrap
                        }
                        RowLayout {
                            ShadowButton {
                                compact: true
                                text: qsTr("Choose image")
                                onClicked: avatarFileDialog.open()
                            }
                            ShadowButton {
                                compact: true
                                variant: ShadowButton.Ghost
                                text: qsTr("Remove")
                                enabled: String(root.profile.avatarUrl).length > 0
                                onClicked: root.profile.clearAvatar()
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.border
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    Label {
                        text: qsTr("Nickname")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    TextField {
                        objectName: "personalProfileNicknameField"
                        Layout.fillWidth: true
                        text: root.nicknameDraft
                        placeholderText: qsTr("How Shadow should address you")
                        font.pixelSize: Theme.fontBody
                        selectByMouse: true
                        onTextEdited: root.nicknameDraft = text
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    Label {
                        text: qsTr("Living places")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Add the cities that belong to ordinary life—home, hometown, or a former home. Optional month ranges decide whether the same city counts as Travel before or after you lived there.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.Wrap
                    }
                    PersonalLivingPlacesEditor {
                        id: livingPlacesEditor
                        objectName: "personalProfileLivingPlacesEditor"
                        Layout.fillWidth: true
                        locationSearch: root.locationSearch
                        libraryCandidates: root.controller.livingPlaceCandidates
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.controller.travelCollectionsBusy
                            || root.controller.livingPlaceCandidates.length === 0
                        text: root.controller.travelCollectionsBusy
                            ? qsTr("Refreshing Library places…")
                            : qsTr("No resolved Library city is available yet; offline search still works")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.Wrap
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: String(root.profile.errorText).length > 0
                    text: root.profile.errorText
                    color: Theme.errorText
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.Wrap
                }

                Item { Layout.preferredHeight: 6 }
            }
        }
    }
}
