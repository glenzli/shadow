pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    objectName: "smartCategoryReviewPopup"

    property var workspace: null
    property string photoId: ""
    property string representationId: ""
    property string photoTitle: ""
    property string completionError: ""
    property int modelRevision: 0
    property real anchorRight: 0
    property real anchorTop: 0
    readonly property real viewportMargin: 8
    readonly property real anchorGap: 6
    readonly property bool hasWorkspace:
        workspace !== null && workspace !== undefined
    // This Popup outlives its card briefly while the gallery is reset for an
    // import. Keep every declarative binding below this boundary so a released
    // card cannot turn QML reevaluation into an error loop.
    readonly property var imageUnderstandingController: hasWorkspace
        && workspace.imageUnderstandingController !== null
        && workspace.imageUnderstandingController !== undefined
        ? workspace.imageUnderstandingController : null
    readonly property bool hasImageUnderstandingController:
        imageUnderstandingController !== null
    readonly property bool hasChanges: {
        const revision = modelRevision
        for (let index = 0; index < categoryModel.count; ++index) {
            const option = categoryModel.get(index)
            if (Boolean(option.chosen) !== Boolean(option.originalChosen))
                return revision >= 0
        }
        return false
    }
    readonly property int remainingUncertainCount: {
        const revision = modelRevision
        let count = 0
        for (let index = 0; index < categoryModel.count; ++index) {
            if (Boolean(categoryModel.get(index).uncertain))
                ++count
        }
        return revision >= 0 ? count : 0
    }
    readonly property bool advancedStateMatches: hasImageUnderstandingController
        && imageUnderstandingController.advancedReviewPhotoId === photoId
        && imageUnderstandingController.advancedReviewRepresentationId
            === representationId
    readonly property string advancedDisposition: advancedStateMatches
        ? imageUnderstandingController.advancedReviewDisposition : ""
    readonly property bool photoProposalMatches: hasImageUnderstandingController
        && imageUnderstandingController.photoProposalAvailable
        && imageUnderstandingController.photoProposalPhotoId === photoId
        && imageUnderstandingController.photoProposalRepresentationId
            === representationId

    width: Math.min(388, Math.max(1,
        (parent ? parent.width : 388) - viewportMargin * 2))
    height: Math.min(implicitHeight, Math.max(1,
        (parent ? parent.height : implicitHeight) - viewportMargin * 2))
    padding: 10
    modal: false
    focus: true
    parent: Overlay.overlay
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function reposition() {
        if (!parent)
            return
        const minimumX = viewportMargin
        const maximumX = Math.max(minimumX,
            parent.width - width - viewportMargin)
        x = Math.max(minimumX, Math.min(anchorRight - width, maximumX))

        const minimumY = viewportMargin
        const maximumY = Math.max(minimumY,
            parent.height - height - viewportMargin)
        const aligned = anchorTop
        const above = anchorTop - height - anchorGap
        if (aligned <= maximumY)
            y = Math.max(minimumY, aligned)
        else if (above >= minimumY)
            y = Math.min(maximumY, above)
        else
            y = maximumY
    }

    function openFor(item, workspaceValue, photoIdValue,
                     representationIdValue, titleValue) {
        workspace = workspaceValue
        if (!hasImageUnderstandingController)
            return
        photoId = String(photoIdValue)
        representationId = String(representationIdValue)
        photoTitle = String(titleValue)
        completionError = ""
        categoryModel.clear()
        const options = workspace.smartCategoryController.feedbackCategories(
            photoId, representationId)
        for (let index = 0; index < options.length; ++index) {
            const option = options[index]
            const matched = Boolean(option.matched)
            categoryModel.append({
                "categoryId": String(option.id),
                "categoryName": String(option.name),
                "uncertain": Boolean(option.uncertain),
                "originalChosen": matched,
                "chosen": matched
            })
        }
        ++modelRevision
        imageUnderstandingController.loadPhotoProposal(
            photoId, representationId)
        parent = Overlay.overlay
        const topRight = item.mapToItem(parent, item.width, 0)
        anchorRight = topRight.x
        anchorTop = topRight.y
        reposition()
        open()
        Qt.callLater(root.reposition)
    }

    function releaseOwner(ownerItem) {
        close()
        if (ownerItem)
            parent = ownerItem
        workspace = null
        photoId = ""
        representationId = ""
        photoTitle = ""
        completionError = ""
        categoryModel.clear()
        ++modelRevision
    }

    onWidthChanged: {
        if (visible)
            reposition()
    }
    onHeightChanged: {
        if (visible)
            reposition()
    }

    Connections {
        target: root.parent
        enabled: root.visible

        function onWidthChanged() {
            root.reposition()
        }

        function onHeightChanged() {
            root.reposition()
        }
    }

    function completeReview() {
        if (!hasWorkspace)
            return
        const decisions = []
        for (let index = 0; index < categoryModel.count; ++index) {
            const option = categoryModel.get(index)
            if (!Boolean(option.uncertain)
                    && Boolean(option.chosen)
                        === Boolean(option.originalChosen))
                continue
            decisions.push({
                "categoryId": String(option.categoryId),
                "decision": Boolean(option.chosen) ? 1 : -1
            })
        }
        if (!workspace.smartCategoryController.completeReview(
                photoId, representationId, decisions)) {
            completionError = qsTr(
                "The review could not be saved. Your choices remain open; try again.")
            return
        }
        completionError = ""
        if (advancedStateMatches
                && advancedDisposition !== "accepted"
                && advancedDisposition !== "dismissed"
                && advancedDisposition !== "") {
            imageUnderstandingController.dismissAdvancedReview()
        }
        close()
    }

    ListModel {
        id: categoryModel
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    contentItem: ColumnLayout {
        width: root.width - root.leftPadding - root.rightPadding
        spacing: 8

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            spacing: 2

            Label {
                Layout.fillWidth: true
                text: qsTr("Correct smart categories")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: root.photoTitle
                color: Theme.textMuted
                font.pixelSize: Theme.fontMeta
                elide: Text.ElideMiddle
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Select every category that correctly describes this photo.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontMeta
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.border
        }

        ScrollView {
            id: reviewBodyScroll
            objectName: "smartCategoryReviewBodyScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 72
            Layout.preferredHeight: reviewBody.implicitHeight
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            ColumnLayout {
                id: reviewBody
                width: reviewBodyScroll.availableWidth
                spacing: 8

                ListView {
                    id: categoryList
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(contentHeight, 320)
                    clip: true
                    spacing: 2
                    model: categoryModel

                    delegate: Rectangle {
                        id: categoryRow
                        required property int index
                        required property string categoryId
                        required property string categoryName
                        required property bool uncertain
                        required property bool originalChosen
                        required property bool chosen
                        width: categoryList.width
                        height: 44
                        radius: Theme.compactControlRadius
                        color: categoryRow.uncertain
                            ? Theme.warningSurface : Theme.transparent
                        border.width: categoryRow.uncertain ? 1 : 0
                        border.color: Theme.warningBorder

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 9
                            anchors.rightMargin: 9
                            spacing: 8

                            ShadowCheckBox {
                                Layout.fillWidth: true
                                compact: true
                                text: categoryRow.categoryName
                                checked: categoryRow.chosen
                                accessibleName: qsTr("%1 belongs to this photo")
                                    .arg(categoryRow.categoryName)
                                onToggled: {
                                    if (checked === categoryRow.chosen)
                                        return
                                    categoryModel.setProperty(
                                        categoryRow.index, "chosen", checked)
                                    ++root.modelRevision
                                }
                            }

                            Label {
                                visible: categoryRow.uncertain
                                text: qsTr("NEEDS REVIEW")
                                color: Theme.warningText
                                font.pixelSize: 8
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.6
                            }

                            Label {
                                visible: !categoryRow.uncertain
                                    && categoryRow.originalChosen
                                text: qsTr("CURRENT")
                                color: Theme.textMuted
                                font.pixelSize: 8
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.6
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    text: qsTr("Your corrections are saved locally and help classify similar photos.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontMeta
                    wrapMode: Text.WordWrap
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: proposalContent.implicitHeight + 18
                    radius: Theme.compactControlRadius
                    color: Theme.surfaceSubtle
                    border.width: 1
                    border.color: Theme.border

                    ColumnLayout {
                        id: proposalContent
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 5

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Local photo description")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontMeta
                            font.weight: Font.DemiBold
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.photoProposalMatches
                            text: root.photoProposalMatches
                                ? root.imageUnderstandingController.photoDescription : ""
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.photoProposalMatches
                            text: root.photoProposalMatches
                                ? qsTr("Suggested keywords: %1").arg(
                                    root.imageUnderstandingController
                                        .photoKeywords.join(" · ")) : ""
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !root.photoProposalMatches
                            text: qsTr("This photo has no local description yet. It will be analyzed when it enters the configured background range.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.photoProposalMatches
                                && root.imageUnderstandingController
                                    .photoProposalDisposition === "suggested"

                            Item { Layout.fillWidth: true }

                            ShadowButton {
                                compact: true
                                text: qsTr("Add suggested keywords")
                                onClicked:
                                    root.imageUnderstandingController
                                        .acceptPhotoKeywords()
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: advancedContent.implicitHeight + 18
                    radius: Theme.compactControlRadius
                    color: Theme.surfaceSubtle
                    border.width: 1
                    border.color: Theme.border

                    ColumnLayout {
                        id: advancedContent
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 7

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Advanced local review")
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontMeta
                                    font.weight: Font.DemiBold
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Ask the larger local model to choose only from your enabled categories.")
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontMeta
                                    wrapMode: Text.WordWrap
                                }
                            }

                            ShadowButton {
                                compact: true
                                // The popup can outlive a card during a gallery reset. Do not evaluate
                                // any controller property (even for an invisible control) until the
                                // retained workspace still owns one.
                                visible: root.hasImageUnderstandingController
                                    && (!root.advancedStateMatches
                                        || (!root.imageUnderstandingController.advancedReviewBusy
                                            && root.advancedDisposition.length === 0))
                                enabled: root.hasImageUnderstandingController
                                    && !root.imageUnderstandingController.advancedReviewBusy
                                text: qsTr("Ask model")
                                onClicked:
                                    root.imageUnderstandingController
                                        .requestAdvancedReview(
                                            root.photoId, root.representationId)
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.hasImageUnderstandingController
                                && root.advancedStateMatches
                                && root.imageUnderstandingController.advancedReviewBusy
                            spacing: 7

                            BusyIndicator {
                                Layout.preferredWidth: 18
                                Layout.preferredHeight: 18
                                running: visible
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Reviewing this photo locally…")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontMeta
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.hasImageUnderstandingController
                                && root.advancedDisposition === "matched"
                            spacing: 7

                            Label {
                                Layout.fillWidth: true
                                text: root.advancedDisposition === "matched"
                                    ? qsTr("Suggested category: %1")
                                        .arg(root.imageUnderstandingController
                                            .advancedReviewCategoryName) : ""
                                color: Theme.textPrimary
                                font.pixelSize: Theme.fontMeta
                                font.weight: Font.DemiBold
                            }

                            ShadowButton {
                                objectName: "acceptAdvancedSmartCategorySuggestionButton"
                                compact: true
                                variant: ShadowButton.Primary
                                text: qsTr("Accept suggestion")
                                onClicked: {
                                    const acceptedCategoryId = String(
                                        root.imageUnderstandingController
                                            .advancedReviewCategoryId)
                                    root.imageUnderstandingController
                                        .acceptAdvancedReview()
                                    if (root.imageUnderstandingController
                                            .advancedReviewDisposition !== "accepted")
                                        return
                                    for (let index = 0;
                                         index < categoryModel.count; ++index) {
                                        if (String(categoryModel.get(index).categoryId)
                                                !== acceptedCategoryId)
                                            continue
                                        categoryModel.setProperty(index, "chosen", true)
                                        categoryModel.setProperty(
                                            index, "originalChosen", true)
                                        categoryModel.setProperty(
                                            index, "uncertain", false)
                                        break
                                    }
                                    ++root.modelRevision
                                    if (root.remainingUncertainCount === 0
                                            && !root.hasChanges)
                                        root.close()
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.hasImageUnderstandingController
                                && root.advancedDisposition === "none"
                            text: qsTr("The model found no suitable category. You can still correct the choices above.")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.hasImageUnderstandingController
                                && root.advancedDisposition === "uncertain"
                            text: qsTr("The model is also uncertain. No category was changed.")
                            color: Theme.warningText
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.hasImageUnderstandingController
                                && root.advancedStateMatches
                                && root.imageUnderstandingController
                                    .advancedReviewError.length > 0
                            text: root.advancedStateMatches
                                ? root.imageUnderstandingController
                                    .advancedReviewError : ""
                            color: Theme.dangerText
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }

        Label {
            objectName: "smartCategoryReviewCompletionError"
            Layout.fillWidth: true
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            visible: root.completionError.length > 0
            text: root.completionError
            color: Theme.dangerText
            font.pixelSize: Theme.fontMeta
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            ShadowButton {
                text: qsTr("Cancel")
                variant: ShadowButton.Ghost
                onClicked: root.close()
            }

            ShadowButton {
                objectName: "completeSmartCategoryReviewButton"
                text: qsTr("Complete review")
                variant: ShadowButton.Primary
                enabled: root.hasWorkspace
                    && (root.remainingUncertainCount > 0 || root.hasChanges)
                onClicked: root.completeReview()
            }
        }
    }
}
