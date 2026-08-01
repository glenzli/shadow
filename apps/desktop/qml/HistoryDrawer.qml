pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Drawer {
    id: drawer
    objectName: "historyDrawer"

    required property var historyController
    required property var editor
    required property var hostWindow

    property string photoTitle: ""
    property int scopeIndex: 0
    readonly property string loadedCommitId: editor.versionDraft
        ? editor.editBaseCommitId : ""

    readonly property bool canUsePhotoEditor: editor.active
        && editor.photoId === historyController.photoId

    edge: Qt.RightEdge
    parent: Overlay.overlay
    modal: true
    focus: true
    width: Math.min(470, Math.max(360, hostWindow.width * 0.36))
    height: parent ? parent.height : hostWindow.height
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function present(photoId, title) {
        const normalizedPhotoId = String(photoId || "").trim()
        photoTitle = String(title || "")
        scopeIndex = normalizedPhotoId.length > 0 ? 0 : 1
        historyController.openForPhoto(normalizedPhotoId)
        open()
    }

    function loadPhotoVersion(commitId) {
        if (!canUsePhotoEditor || editor.stateBusy)
            return
        editor.loadVersionDraft(String(commitId))
    }

    function dateText(createdAtMs) {
        const value = Number(createdAtMs)
        return Number.isFinite(value)
            ? new Date(value).toLocaleString(Qt.locale(), Locale.ShortFormat) : ""
    }

    function parameterLabel(key) {
        switch (String(key)) {
        case "exposure_stops": return qsTr("Exposure")
        case "contrast_factor": return qsTr("Contrast")
        case "white_balance_temperature": return qsTr("White balance")
        case "white_balance_tint": return qsTr("Tint")
        case "saturation_factor": return qsTr("Saturation")
        case "highlights": return qsTr("Highlights")
        case "shadows": return qsTr("Shadows")
        case "whites": return qsTr("Whites")
        case "blacks": return qsTr("Blacks")
        case "vibrance": return qsTr("Vibrance")
        case "grade_node_enabled": return qsTr("Node enablement")
        case "oklab_lightness_curve": return qsTr("Lightness curve")
        case "color_warper": return qsTr("Color Warper")
        case "color_mixer_hue": return qsTr("Color Mixer hue")
        case "color_mixer_saturation": return qsTr("Color Mixer saturation")
        case "color_mixer_lightness": return qsTr("Color Mixer lightness")
        case "color_range": return qsTr("Color range")
        case "selective_color": return qsTr("Selective Color")
        case "lut": return qsTr("LUT")
        case "sharpening": return qsTr("Sharpening")
        case "optics": return qsTr("Optics")
        default: return qsTr("Adjustment")
        }
    }

    function photoChangeSummary(
        isRoot,
        gradeNodesAdded,
        gradeNodesRemoved,
        gradeNodesMoved,
        gradeNodesModified,
        renderOpsAdded,
        renderOpsRemoved,
        renderOpsModified,
        parameterBlocksChanged,
        changedParameterKeys,
        schemaChanged,
        otherChanges
    ) {
        if (isRoot)
            return qsTr("Initial edit state")
        const parts = []
        const gradeChanges = Number(gradeNodesAdded) + Number(gradeNodesRemoved)
            + Number(gradeNodesMoved) + Number(gradeNodesModified)
        const operationChanges = Number(renderOpsAdded) + Number(renderOpsRemoved)
            + Number(renderOpsModified)
        if (gradeChanges > 0)
            parts.push(qsTr("%L1 node changes").arg(gradeChanges))
        if (changedParameterKeys && changedParameterKeys.length > 0) {
            const labels = []
            for (let index = 0; index < changedParameterKeys.length && index < 3; ++index)
                labels.push(parameterLabel(changedParameterKeys[index]))
            if (changedParameterKeys.length > 3)
                labels.push(qsTr("+%L1 more").arg(changedParameterKeys.length - 3))
            parts.push(labels.join(qsTr(", ")))
        } else if (Number(parameterBlocksChanged) > 0) {
            parts.push(qsTr("%L1 parameter groups").arg(parameterBlocksChanged))
        }
        if (operationChanges > 0)
            parts.push(qsTr("%L1 operation changes").arg(operationChanges))
        if (schemaChanged)
            parts.push(qsTr("Recipe structure changed"))
        if (otherChanges)
            parts.push(qsTr("Foundation, masks, retouch, Liquify, or geometry changes"))
        return parts.length > 0 ? parts.join(qsTr(" · "))
            : qsTr("No pixel-setting changes")
    }

    function libraryChangeSummary(
        photoChanges,
        sharedGradeChanges,
        maskChanges,
        styleChanges,
        outputStateChanges,
        isRoot
    ) {
        if (isRoot)
            return qsTr("Initial Library edit state")
        const parts = []
        if (Number(photoChanges) > 0)
            parts.push(qsTr("%L1 photos").arg(photoChanges))
        if (Number(sharedGradeChanges) > 0)
            parts.push(qsTr("%L1 shared nodes").arg(sharedGradeChanges))
        if (Number(maskChanges) > 0)
            parts.push(qsTr("%L1 masks").arg(maskChanges))
        if (Number(styleChanges) > 0)
            parts.push(qsTr("%L1 styles").arg(styleChanges))
        if (Number(outputStateChanges) > 0)
            parts.push(qsTr("%L1 output states").arg(outputStateChanges))
        return parts.length > 0 ? parts.join(qsTr(" · "))
            : qsTr("No Library entity changes")
    }

    background: Rectangle {
        color: Theme.panel
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 54
            Layout.leftMargin: 18
            Layout.rightMargin: 12
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1

                Label {
                    text: qsTr("HISTORY")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSection
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.1
                }

                Label {
                    Layout.fillWidth: true
                    text: drawer.scopeIndex === 0 && drawer.photoTitle.length > 0
                        ? drawer.photoTitle : qsTr("Non-destructive edit history")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    elide: Text.ElideRight
                }
            }

            ShadowIconButton {
                source: "qrc:/icons/close.svg"
                toolTipText: qsTr("Close History")
                accessibleName: toolTipText
                onClicked: drawer.close()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 4

            ShadowTabButton {
                objectName: "historyPhotoTab"
                Layout.fillWidth: true
                active: drawer.scopeIndex === 0
                text: qsTr("PHOTO")
                enabled: drawer.historyController.photoId.length > 0
                onClicked: drawer.scopeIndex = 0
            }

            ShadowTabButton {
                objectName: "historyLibraryTab"
                Layout.fillWidth: true
                active: drawer.scopeIndex === 1
                text: qsTr("LIBRARY")
                onClicked: drawer.scopeIndex = 1
            }

            ShadowIconButton {
                source: "qrc:/icons/reset-all.svg"
                toolTipText: qsTr("Refresh History")
                accessibleName: toolTipText
                enabled: drawer.scopeIndex === 0
                    ? !drawer.historyController.photoBusy
                    : !drawer.historyController.libraryBusy
                        && !drawer.historyController.libraryRefsBusy
                onClicked: {
                    if (drawer.scopeIndex === 0) {
                        drawer.historyController.refreshPhoto()
                    } else {
                        drawer.historyController.refreshLibrary()
                        drawer.historyController.refreshLibraryRefs()
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: drawer.scopeIndex

            ColumnLayout {
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: versionComposer.visible
                        ? versionComposer.implicitHeight + 24 : 0
                    visible: drawer.canUsePhotoEditor
                    color: Theme.panelRaised

                    ColumnLayout {
                        id: versionComposer
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 12
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Create a named version")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                            font.weight: Font.Medium
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Captures the complete current Recipe. Autosaves remain in the timeline without creating named versions.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            TextField {
                                id: versionNameInput
                                objectName: "historyVersionNameInput"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Version name")
                                selectByMouse: true
                                enabled: !drawer.editor.stateBusy
                                onAccepted: createVersionButton.clicked()
                            }

                            ShadowButton {
                                id: createVersionButton
                                objectName: "historyCreateVersionButton"
                                compact: true
                                variant: ShadowButton.Primary
                                text: qsTr("Create")
                                enabled: versionNameInput.text.trim().length > 0
                                    && drawer.canUsePhotoEditor
                                    && !drawer.editor.stateBusy
                                onClicked: {
                                    drawer.editor.saveVersion(versionNameInput.text.trim())
                                    versionNameInput.clear()
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    visible: drawer.canUsePhotoEditor
                    color: Theme.border
                }

                ListView {
                    id: photoHistoryList
                    objectName: "photoHistoryList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 12
                    model: drawer.historyController.photoModel
                    spacing: 8
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        id: photoRow

                        required property string commitId
                        required property string name
                        required property double createdAtMs
                        required property var parentCommitIds
                        required property var refs
                        required property bool isNamed
                        required property bool isWorking
                        required property bool isRoot
                        required property bool recipeSchemaChanged
                        required property int gradeNodesAdded
                        required property int gradeNodesRemoved
                        required property int gradeNodesMoved
                        required property int gradeNodesModified
                        required property int renderOpsAdded
                        required property int renderOpsRemoved
                        required property int renderOpsModified
                        required property int parameterBlocksChanged
                        required property var changedParameterKeys
                        required property bool hasOtherChanges

                        readonly property bool loaded: drawer.loadedCommitId === commitId
                        readonly property bool canLoad: drawer.canUsePhotoEditor
                            && !drawer.editor.stateBusy && !loaded
                        width: photoHistoryList.width
                        height: photoRowContent.implicitHeight + 22
                        radius: Theme.controlRadius
                        color: loaded || isWorking
                            ? Theme.currentRevisionSurface : Theme.panelRaised
                        border.width: 1
                        border.color: loaded || isWorking
                            ? Theme.currentRevisionBorder : Theme.border

                        ColumnLayout {
                            id: photoRowContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 5

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7

                                Label {
                                    Layout.fillWidth: true
                                    text: photoRow.name.length > 0
                                        ? photoRow.name
                                        : photoRow.isWorking
                                            ? qsTr("Autosaved working copy")
                                            : qsTr("Autosave")
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontBody
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }

                                Label {
                                    text: photoRow.loaded ? qsTr("LOADED")
                                        : photoRow.isWorking ? qsTr("CURRENT")
                                        : photoRow.isNamed ? qsTr("VERSION") : qsTr("AUTO")
                                    color: photoRow.loaded || photoRow.isWorking
                                        ? Theme.accent : Theme.textMuted
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0.6
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: drawer.photoChangeSummary(
                                    photoRow.isRoot,
                                    photoRow.gradeNodesAdded,
                                    photoRow.gradeNodesRemoved,
                                    photoRow.gradeNodesMoved,
                                    photoRow.gradeNodesModified,
                                    photoRow.renderOpsAdded,
                                    photoRow.renderOpsRemoved,
                                    photoRow.renderOpsModified,
                                    photoRow.parameterBlocksChanged,
                                    photoRow.changedParameterKeys,
                                    photoRow.recipeSchemaChanged,
                                    photoRow.hasOtherChanges
                                )
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontMeta
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%1 · %L2 parent(s)")
                                    .arg(drawer.dateText(photoRow.createdAtMs))
                                    .arg(photoRow.parentCommitIds.length)
                                color: Theme.textMuted
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            objectName: "loadPhotoHistory-" + photoRow.commitId
                            anchors.fill: parent
                            enabled: photoRow.canLoad
                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: drawer.loadPhotoVersion(photoRow.commitId)
                        }
                    }

                    footer: ColumnLayout {
                        width: photoHistoryList.width
                        spacing: 8

                        ShadowButton {
                            Layout.alignment: Qt.AlignHCenter
                            visible: drawer.historyController.photoHasMore
                            compact: true
                            text: qsTr("Load older edits")
                            enabled: !drawer.historyController.photoBusy
                            onClicked: drawer.historyController.loadMorePhoto()
                        }

                        Item { Layout.preferredHeight: 4 }
                    }

                    Label {
                        anchors.centerIn: parent
                        width: Math.max(0, parent.width - 32)
                        visible: photoHistoryList.count === 0
                            && !drawer.historyController.photoBusy
                            && drawer.historyController.photoErrorText.length === 0
                        text: drawer.historyController.photoId.length === 0
                            ? qsTr("Select or open a photo to see its edit history.")
                            : qsTr("This photo has no durable edit history yet.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        visible: drawer.historyController.photoBusy
                            && photoHistoryList.count === 0
                        running: visible
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.bottomMargin: 12
                    visible: drawer.historyController.photoErrorText.length > 0
                    text: drawer.historyController.photoErrorText
                    color: Theme.errorText
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }
            }

            ColumnLayout {
                spacing: 0

                ListView {
                    id: libraryHistoryList
                    objectName: "libraryHistoryList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 12
                    model: drawer.historyController.libraryModel
                    spacing: 8
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: Rectangle {
                        id: libraryRow

                        required property string commitId
                        required property string message
                        required property double createdAtMs
                        required property var parentCommitIds
                        required property var refs
                        required property bool isRoot
                        required property bool isHead
                        required property int photoChanges
                        required property int sharedGradeChanges
                        required property int maskChanges
                        required property int styleChanges
                        required property int outputStateChanges
                        required property int totalChanges

                        width: libraryHistoryList.width
                        height: libraryRowContent.implicitHeight + 22
                        radius: Theme.controlRadius
                        color: libraryRow.isHead
                            ? Theme.currentRevisionSurface : Theme.panelRaised
                        border.width: 1
                        border.color: libraryRow.isHead
                            ? Theme.currentRevisionBorder : Theme.border

                        ColumnLayout {
                            id: libraryRowContent
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 5

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7

                                Label {
                                    Layout.fillWidth: true
                                    text: libraryRow.message.length > 0
                                        ? libraryRow.message : qsTr("Library checkpoint")
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontBody
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }

                                Label {
                                    text: libraryRow.isHead ? qsTr("HEAD")
                                        : libraryRow.isRoot ? qsTr("ROOT") : ""
                                    visible: text.length > 0
                                    color: libraryRow.isHead ? Theme.accent : Theme.textMuted
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0.6
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: drawer.libraryChangeSummary(
                                    libraryRow.photoChanges,
                                    libraryRow.sharedGradeChanges,
                                    libraryRow.maskChanges,
                                    libraryRow.styleChanges,
                                    libraryRow.outputStateChanges,
                                    libraryRow.isRoot
                                )
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontMeta
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%1 · %L2 ref(s) · %L3 parent(s)")
                                    .arg(drawer.dateText(libraryRow.createdAtMs))
                                    .arg(libraryRow.refs.length)
                                    .arg(libraryRow.parentCommitIds.length)
                                color: Theme.textMuted
                                font.pixelSize: 9
                                elide: Text.ElideRight
                            }
                        }
                    }

                    footer: ColumnLayout {
                        width: libraryHistoryList.width
                        spacing: 8

                        ShadowButton {
                            Layout.alignment: Qt.AlignHCenter
                            visible: drawer.historyController.libraryHasMore
                            compact: true
                            text: qsTr("Load older Library commits")
                            enabled: !drawer.historyController.libraryBusy
                            onClicked: drawer.historyController.loadMoreLibrary()
                        }

                        Item { Layout.preferredHeight: 4 }
                    }

                    Label {
                        anchors.centerIn: parent
                        width: Math.max(0, parent.width - 32)
                        visible: libraryHistoryList.count === 0
                            && !drawer.historyController.libraryBusy
                            && drawer.historyController.libraryErrorText.length === 0
                        text: qsTr("No Library-wide edit commits yet.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        visible: drawer.historyController.libraryBusy
                            && libraryHistoryList.count === 0
                        running: visible
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    visible: drawer.historyController.libraryErrorText.length > 0
                    text: drawer.historyController.libraryErrorText
                    color: Theme.errorText
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.border
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(180, Math.max(112, implicitHeight))
                    Layout.leftMargin: 14
                    Layout.rightMargin: 14
                    Layout.topMargin: 10
                    Layout.bottomMargin: 12
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("REFERENCES")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontMeta
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.8
                        }

                        BusyIndicator {
                            implicitWidth: 18
                            implicitHeight: 18
                            visible: drawer.historyController.libraryRefsBusy
                            running: visible
                        }
                    }

                    ListView {
                        id: refList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: drawer.historyController.libraryRefs
                        spacing: 4
                        clip: true
                        orientation: ListView.Vertical

                        delegate: RowLayout {
                            required property var modelData

                            width: refList.width
                            spacing: 7

                            Label {
                                text: String(modelData.kind) === "branch" ? qsTr("BRANCH")
                                    : String(modelData.kind) === "tag" ? qsTr("TAG")
                                    : qsTr("VERSION")
                                color: Theme.accent
                                font.pixelSize: 9
                                font.weight: Font.Bold
                                font.letterSpacing: 0.5
                            }

                            Label {
                                Layout.fillWidth: true
                                text: String(modelData.name)
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontMeta
                                elide: Text.ElideMiddle
                            }
                        }
                    }

                    ShadowButton {
                        Layout.alignment: Qt.AlignHCenter
                        visible: drawer.historyController.libraryRefsHaveMore
                        compact: true
                        text: qsTr("Load more references")
                        enabled: !drawer.historyController.libraryRefsBusy
                        onClicked: drawer.historyController.loadMoreLibraryRefs()
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: drawer.historyController.libraryRefsErrorText.length > 0
                        text: drawer.historyController.libraryRefsErrorText
                        color: Theme.errorText
                        font.pixelSize: Theme.fontMeta
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    Connections {
        target: drawer.editor

        function onStateBusyChanged() {
            if (drawer.editor.stateBusy || !drawer.opened)
                return
            drawer.historyController.refreshAll()
        }
    }
}
