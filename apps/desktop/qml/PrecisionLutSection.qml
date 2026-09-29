pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ShadowAdjustmentSection {
    id: lutSection

    required property var editor
    required property var lutLibrary
    required property color textPrimary
    required property color textSecondary
    required property color textMuted
    required property color accent

    signal openLibraryRequested()

    property bool browserExpanded: false
    readonly property var browserGroups: buildLutBrowserGroups(
        lutLibrary ? lutLibrary.availableEntries : [])

    function lutPathSegment(path) {
        const parts = String(path || "").split("/")
        return parts.length > 0 ? parts[parts.length - 1] : String(path || "")
    }

    function lutRelativeDirectory(entry) {
        const sourceRoot = String(entry.directory || "")
        const absolutePath = String(entry.path || "")
        const prefix = sourceRoot.length > 0 ? sourceRoot + "/" : ""
        const relativePath = absolutePath.indexOf(prefix) === 0
            ? absolutePath.slice(prefix.length) : String(entry.fileName || "")
        const slash = relativePath.lastIndexOf("/")
        return slash > 0 ? relativePath.slice(0, slash) : ""
    }

    function buildLutBrowserGroups(entries) {
        const groupsByPath = ({})
        for (let index = 0; index < entries.length; ++index) {
            const entry = entries[index]
            const sourceRoot = String(entry.directory || "")
            const relativePath = lutRelativeDirectory(entry)
            const key = sourceRoot + "\u001f" + relativePath
            if (!groupsByPath[key]) {
                groupsByPath[key] = {
                    title: relativePath.length > 0
                        ? lutPathSegment(sourceRoot) + " / " + relativePath
                        : lutPathSegment(sourceRoot),
                    entries: []
                }
            }
            groupsByPath[key].entries.push(entry)
        }

        const groups = []
        for (const key in groupsByPath)
            groups.push(groupsByPath[key])
        groups.sort((left, right) => left.title.localeCompare(right.title))
        return groups
    }

    function selectLut(entry) {
        editor.setLutResource(entry.id, entry.title, entry.managedPath)
        browserExpanded = false
    }

    function clearSelection() {
        editor.clearLut()
    }

    function beginIntensityEdit() {
        editor.beginParameterEdit("lut_intensity")
    }

    function setIntensity(value) {
        editor.lutIntensity = value
    }

    function endIntensityEdit() {
        editor.endParameterEdit("lut_intensity")
    }

    Layout.fillWidth: true
    title: qsTr("LUT")
    summary: editor.hasLut ? editor.lutTitle : qsTr("None")
    toolTipText: qsTr("Apply a 3D .cube LUT in linear sRGB. External LUTs must expect this input space. The library button opens LUT management.")
    resetAvailable: true
    onResetRequested: editor.resetSelectedAdjustmentSection("lut")

    Label {
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        text: qsTr("Input / output: linear sRGB")
        color: lutSection.textMuted
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        spacing: 6

        Rectangle {
            id: lutSelector

            Layout.fillWidth: true
            Layout.preferredHeight: 52
            radius: Theme.controlRadius
            color: lutSelectorMouse.pressed
                ? Theme.buttonPressedSurface
                : lutSelectorMouse.containsMouse
                    ? Theme.buttonHoverSurface
                    : Theme.buttonSurface
            border.width: 1
            border.color: lutSelectorMouse.containsMouse
                ? Theme.borderStrong : Theme.buttonBorder

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 9
                spacing: 8

                Rectangle {
                    Layout.preferredWidth: 62
                    Layout.preferredHeight: 38
                    radius: Theme.compactControlRadius
                    clip: true
                    color: Theme.photoCanvas
                    border.color: Theme.border

                    Image {
                        anchors.fill: parent
                        source: "image://shadow-lut/"
                            + (lutSection.editor.hasLut
                                ? lutSection.editor.lutResourceId
                                : "original")
                        sourceSize.width: 124
                        sourceSize.height: 76
                        asynchronous: true
                        cache: true
                        fillMode: Image.PreserveAspectCrop
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: lutSection.editor.hasLut
                        ? lutSection.editor.lutTitle
                        : qsTr("Choose a LUT")
                    color: lutSection.editor.hasLut
                        ? lutSection.textPrimary
                        : lutSection.textMuted
                    font.pixelSize: Theme.fontMeta
                    elide: Text.ElideRight
                }

                ShadowIcon {
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 14
                    size: 14
                    source: "qrc:/icons/chevron-down.svg"
                    color: lutSection.textSecondary
                    rotation: lutSection.browserExpanded ? 180 : 0
                }
            }

            MouseArea {
                id: lutSelectorMouse

                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                enabled: lutSection.editor.active
                    && !lutSection.editor.stateBusy
                onClicked: lutSection.browserExpanded
                    = !lutSection.browserExpanded
            }
        }

        ShadowIconButton {
            Layout.preferredWidth: Theme.controlHeight
            Layout.preferredHeight: Theme.controlHeight
            Layout.alignment: Qt.AlignVCenter
            buttonSize: Theme.controlHeight
            variant: ShadowIconButton.Secondary
            source: "qrc:/icons/library-manage.svg"
            toolTipText: qsTr("Manage LUT Library")
            onClicked: lutSection.openLibraryRequested()
        }

        ShadowIconButton {
            visible: lutSection.editor.hasLut
            Layout.preferredWidth: visible ? Theme.controlHeight : 0
            Layout.preferredHeight: Theme.controlHeight
            Layout.alignment: Qt.AlignVCenter
            buttonSize: Theme.controlHeight
            variant: ShadowIconButton.Ghost
            source: "qrc:/icons/clear.svg"
            toolTipText: qsTr("Remove LUT from this Grade Node")
            onClicked: lutSection.clearSelection()
        }
    }

    ColumnLayout {
        visible: lutSection.browserExpanded
            && lutSection.browserGroups.length > 0
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.preferredHeight: visible ? implicitHeight : 0
        spacing: 10

        Repeater {
            model: lutSection.browserGroups

            delegate: ColumnLayout {
                id: lutGroup

                required property var modelData
                property bool expanded: false

                Layout.fillWidth: true
                spacing: 6

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    radius: Theme.compactControlRadius
                    color: lutGroupHeaderMouse.containsMouse
                        ? Theme.buttonGhostHover
                        : Theme.surfaceSubtle
                    border.color: Theme.border

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: lutGroup.modelData.title
                            color: lutSection.textSecondary
                            font.pixelSize: Theme.fontCaption
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }

                        Label {
                            text: String(lutGroup.modelData.entries.length)
                            color: lutSection.textMuted
                            font.pixelSize: Theme.fontCaption
                        }

                        ShadowIcon {
                            size: 12
                            source: "qrc:/icons/chevron-down.svg"
                            color: lutSection.textMuted
                            rotation: lutGroup.expanded ? 180 : 0
                        }
                    }

                    MouseArea {
                        id: lutGroupHeaderMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: lutGroup.expanded = !lutGroup.expanded
                    }
                }

                GridLayout {
                    visible: lutGroup.expanded
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    columns: width >= 296 ? 2 : 1
                    columnSpacing: 6
                    rowSpacing: 6

                    Repeater {
                        model: lutGroup.expanded
                            ? lutGroup.modelData.entries : []

                        delegate: Rectangle {
                            id: lutCard

                            required property var modelData
                            readonly property bool current:
                                lutSection.editor.lutResourceId
                                    === modelData.id

                            Layout.fillWidth: true
                            Layout.preferredHeight: 86
                            radius: Theme.compactControlRadius
                            color: current
                                ? Theme.accentSurfaceQuiet
                                : lutCardMouse.containsMouse
                                    ? Theme.buttonHoverSurface
                                    : Theme.buttonSurface
                            border.color: current
                                ? Theme.accentBorder
                                : Theme.buttonBorder

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 7

                                Rectangle {
                                    Layout.preferredWidth: 64
                                    Layout.preferredHeight: 72
                                    radius: Theme.compactControlRadius
                                    clip: true
                                    color: Theme.photoCanvas
                                    border.color: lutCard.current
                                        ? Theme.accentBorder
                                        : Theme.border

                                    Image {
                                        anchors.fill: parent
                                        source: "image://shadow-lut/"
                                            + lutCard.modelData.id
                                        sourceSize.width: 128
                                        sourceSize.height: 144
                                        asynchronous: true
                                        cache: true
                                        fillMode: Image.PreserveAspectCrop
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: lutCard.modelData.title
                                        color: lutCard.current
                                            ? lutSection.accent
                                            : lutSection.textPrimary
                                        font.pixelSize: Theme.fontMeta
                                        font.weight: lutCard.current
                                            ? Font.DemiBold : Font.Medium
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("%1³").arg(
                                            lutCard.modelData.size)
                                        color: lutSection.textMuted
                                        font.pixelSize: Theme.fontCaption
                                    }
                                }
                            }

                            MouseArea {
                                id: lutCardMouse

                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    lutSection.selectLut(lutCard.modelData)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    ShadowSlider {
        visible: lutSection.editor.hasLut
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        label: qsTr("Intensity")
        from: 0
        to: 1
        neutralValue: 1
        stepSize: 0.01
        decimals: 0
        displayMultiplier: 100
        suffix: "%"
        value: lutSection.editor.lutIntensity
        onGestureStarted: lutSection.beginIntensityEdit()
        onEdited: value => lutSection.setIntensity(value)
        onGestureFinished: lutSection.endIntensityEdit()
    }
}
