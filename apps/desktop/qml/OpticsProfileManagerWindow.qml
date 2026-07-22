pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "opticsProfileManagerWindow"
    required property var editor
    property var candidates: []
    property string query: ""

    width: 720
    height: 560
    minimumWidth: 560
    minimumHeight: 420
    title: qsTr("Optical Profile Library")
    color: Theme.window
    visible: false
    modality: Qt.NonModal

    function openManager() {
        candidates = editor.opticsProfileCandidates()
        show()
        raise()
        requestActivate()
    }

    function matches(entry) {
        const needle = query.trim().toLowerCase()
        return needle.length === 0
            || entry.cameraMaker.toLowerCase().includes(needle)
            || entry.cameraModel.toLowerCase().includes(needle)
            || entry.lensMaker.toLowerCase().includes(needle)
            || entry.lensModel.toLowerCase().includes(needle)
    }

    function matchingCandidateCount() {
        let count = 0
        for (const candidate of candidates) {
            if (matches(candidate))
                ++count
        }
        return count
    }

    header: ToolBar {
        implicitHeight: 58
        background: Rectangle {
            color: Theme.chrome
            Rectangle { anchors.left: parent.left; anchors.right: parent.right
                anchors.bottom: parent.bottom; height: 1; color: Theme.border }
        }
        contentItem: RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 14
            spacing: 12
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    text: qsTr("OPTICAL PROFILE LIBRARY")
                    color: Theme.textPrimary
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }
                Label {
                    readonly property var receipt: root.editor.opticsReceipt
                    text: (root.editor.opticsManualProfile
                        ? qsTr("Manual · %1").arg(root.editor.opticsLensProfile)
                        : qsTr("Automatic profile matching"))
                        + (receipt.valid && receipt.providerVersion.length > 0
                            ? " · " + receipt.providerVersion : "")
                    color: Theme.textMuted
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
            }
            ShadowButton {
                text: qsTr("Use Automatic")
                enabled: root.editor.opticsManualProfile
                variant: ShadowButton.Secondary
                onClicked: root.editor.clearManualOpticsProfile()
            }
            ShadowIconButton {
                source: "qrc:/icons/redo.svg"
                toolTipText: qsTr("Reload Lensfun profiles")
                accessibleName: toolTipText
                onClicked: root.candidates = root.editor.opticsProfileCandidates()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        TextField {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            placeholderText: qsTr("Search camera or lens")
            color: Theme.textPrimary
            placeholderTextColor: Theme.textPlaceholder
            selectByMouse: true
            onTextChanged: root.query = text
            background: Rectangle {
                radius: Theme.controlRadius
                color: Theme.control
                border.color: parent.activeFocus ? Theme.focusRing : Theme.border
            }
        }

        ListView {
            id: profileList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: root.candidates
            delegate: Rectangle {
                id: profileRow
                required property var modelData
                width: ListView.view.width
                height: root.matches(modelData) ? 66 : 0
                visible: height > 0
                radius: Theme.controlRadius
                readonly property bool selected: root.editor.opticsManualProfile
                    && root.editor.opticsCameraProfile === modelData.cameraModel
                    && root.editor.opticsLensProfile === modelData.lensModel
                color: selected ? Theme.accentSurface : rowMouse.containsMouse
                    ? Theme.surfaceSelected : Theme.panel
                border.color: selected ? Theme.accentBorder : Theme.border

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 10
                    spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: profileRow.modelData.lensModel
                            color: Theme.textPrimary
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: [profileRow.modelData.lensMaker,
                                profileRow.modelData.cameraMaker,
                                profileRow.modelData.cameraModel].filter(value => value.length > 0).join(" · ")
                            color: Theme.textMuted
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }
                    Label {
                        visible: profileRow.selected
                        text: qsTr("ACTIVE")
                        color: Theme.accent
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.6
                    }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onDoubleClicked: {
                        root.editor.setManualOpticsProfile(
                            profileRow.modelData.cameraMaker,
                            profileRow.modelData.cameraModel,
                            profileRow.modelData.lensMaker,
                            profileRow.modelData.lensModel)
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 48
                visible: root.matchingCandidateCount() === 0
                text: root.query.trim().length > 0
                    ? qsTr("No optical profiles match this search.")
                    : qsTr("No compatible Lensfun profiles were found for this camera.")
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
        }

        Label {
            Layout.fillWidth: true
            text: root.candidates.length > 0
                ? qsTr("%1 compatible profiles · Double-click one to use it. The selection is saved with the edit recipe and can be undone.").arg(root.candidates.length)
                : qsTr("Double-click a profile to use it. The selection is saved with the edit recipe and can be undone.")
            color: Theme.textMuted
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }
    }
}
