pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "opticsProfileManagerWindow"
    required property var editor
    required property var opticsProfileLibrary
    property var candidates: []
    property string query: ""
    property int sourceIndex: 0

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
        opticsProfileLibrary.rescan()
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
                visible: root.sourceIndex === 0
                text: qsTr("Use Automatic")
                enabled: root.editor.opticsManualProfile
                variant: ShadowButton.Secondary
                onClicked: root.editor.clearManualOpticsProfile()
            }
            ShadowIconButton {
                visible: root.sourceIndex === 0
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

        TabBar {
            id: sourceTabs
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            currentIndex: root.sourceIndex
            background: Rectangle {
                radius: Theme.controlRadius
                color: Theme.control
                border.color: Theme.border
            }
            ShadowTabButton {
                text: qsTr("Lensfun")
                checked: sourceTabs.currentIndex === 0
                onClicked: root.sourceIndex = 0
            }
            ShadowTabButton {
                text: qsTr("My Profiles")
                checked: sourceTabs.currentIndex === 1
                onClicked: root.sourceIndex = 1
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                visible: root.sourceIndex === 0
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

            ColumnLayout {
                anchors.fill: parent
                visible: root.sourceIndex === 1
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Save the current manual optics values as a reusable local profile. Applying one copies its values into this photo's recipe.")
                    color: Theme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    TextField {
                        id: customProfileTitle
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        placeholderText: qsTr("Profile name")
                        color: Theme.textPrimary
                        placeholderTextColor: Theme.textPlaceholder
                        selectByMouse: true
                        background: Rectangle {
                            radius: Theme.controlRadius
                            color: Theme.control
                            border.color: parent.activeFocus ? Theme.focusRing : Theme.border
                        }
                    }
                    TextField {
                        id: customProfileLens
                        Layout.preferredWidth: 190
                        Layout.preferredHeight: 34
                        placeholderText: qsTr("Lens label (optional)")
                        color: Theme.textPrimary
                        placeholderTextColor: Theme.textPlaceholder
                        selectByMouse: true
                        background: Rectangle {
                            radius: Theme.controlRadius
                            color: Theme.control
                            border.color: parent.activeFocus ? Theme.focusRing : Theme.border
                        }
                    }
                    ShadowButton {
                        text: qsTr("Save Current")
                        variant: ShadowButton.Primary
                        enabled: root.editor.active && customProfileTitle.text.trim().length > 0
                        onClicked: {
                            if (root.opticsProfileLibrary.saveProfile(
                                    customProfileTitle.text,
                                    customProfileLens.text,
                                    {
                                        "manualDistortion": Number(root.editor.manualOpticsDistortion),
                                        "manualTcaRedCyan": Number(root.editor.manualOpticsTcaRedCyan),
                                        "manualTcaBlueYellow": Number(root.editor.manualOpticsTcaBlueYellow),
                                        "manualVignettingAmount": Number(root.editor.manualOpticsVignettingAmount),
                                        "manualVignettingMidpoint": Number(root.editor.manualOpticsVignettingMidpoint)
                                    })) {
                                customProfileTitle.clear()
                                customProfileLens.clear()
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.opticsProfileLibrary.lastError.length > 0
                    text: root.opticsProfileLibrary.lastError
                    color: Theme.dangerText
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }

                ListView {
                    id: customProfileList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 6
                    model: root.opticsProfileLibrary.entries
                    delegate: Rectangle {
                        id: customProfileRow
                        required property var modelData
                        width: ListView.view.width
                        height: 68
                        radius: Theme.controlRadius
                        readonly property bool matchesCurrent: root.editor.manualOpticsDistortion
                                === modelData.manualDistortion
                            && root.editor.manualOpticsTcaRedCyan
                                === modelData.manualTcaRedCyan
                            && root.editor.manualOpticsTcaBlueYellow
                                === modelData.manualTcaBlueYellow
                            && root.editor.manualOpticsVignettingAmount
                                === modelData.manualVignettingAmount
                            && root.editor.manualOpticsVignettingMidpoint
                                === modelData.manualVignettingMidpoint
                        color: matchesCurrent ? Theme.accentSurface : Theme.panel
                        border.color: matchesCurrent ? Theme.accentBorder : Theme.border

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 10
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    Layout.fillWidth: true
                                    text: customProfileRow.modelData.title
                                    color: Theme.textPrimary
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: customProfileRow.modelData.lensLabel.length > 0
                                        ? customProfileRow.modelData.lensLabel
                                        : qsTr("Manual optical correction")
                                    color: Theme.textMuted
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                            }
                            Label {
                                visible: customProfileRow.matchesCurrent
                                text: qsTr("MATCHES CURRENT")
                                color: Theme.accent
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.6
                            }
                            ShadowIconButton {
                                source: "qrc:/icons/check.svg"
                                toolTipText: qsTr("Apply this local optical profile")
                                accessibleName: toolTipText
                                onClicked: root.editor.applyManualOpticsProfile(
                                    customProfileRow.modelData)
                            }
                            ShadowIconButton {
                                source: "qrc:/icons/trash.svg"
                                toolTipText: qsTr("Remove this local optical profile")
                                accessibleName: toolTipText
                                variant: ShadowIconButton.Danger
                                onClicked: root.opticsProfileLibrary.removeProfile(
                                    customProfileRow.modelData.id)
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 48
                        visible: root.opticsProfileLibrary.count === 0
                        text: qsTr("No local optical profiles yet. Save the current manual correction to reuse it on another photo.")
                        color: Theme.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Stored locally as data-only profile files. Updating or removing one never changes photos that already use its values.")
                    color: Theme.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
