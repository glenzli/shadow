pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: review

    required property var controller
    property string selectedPhotoId: ""
    property string selectedRepresentationId: ""
    property string selectedVisualHandle: ""
    property var selectedDecisionHeadSequence: 0
    property string selectedDecisionFlag: "unflagged"
    property int selectedDecisionRating: 0
    property string selectedTitle: ""
    property string selectedPath: ""
    property string selectedRole: ""
    property string selectedVisualSource: ""
    property int selectedWidth: 0
    property int selectedHeight: 0
    property bool selectedHasTechnicalObservation: false
    property int selectedTechnicalInputWidth: 0
    property int selectedTechnicalInputHeight: 0
    property string selectedTechnicalPreprocessingVersion: ""
    property string selectedTechnicalImplementationVersion: ""
    property real selectedMeanLuma: 0.0
    property real selectedP01Luma: 0.0
    property real selectedP50Luma: 0.0
    property real selectedP99Luma: 0.0
    property real selectedNearBlackFraction: 0.0
    property real selectedNearWhiteFraction: 0.0
    property real selectedLaplacianVariance: 0.0
    property real selectedEdgeEnergy: 0.0
    property var leftComparisonSnapshot: null
    property var rightComparisonSnapshot: null
    property bool leftComparisonVisualReady: false
    property bool rightComparisonVisualReady: false
    property bool comparisonBackendReady: false
    property string comparisonPresentationId: ""
    property string leftComparisonRequestTicket: ""
    property string rightComparisonRequestTicket: ""
    property string leftComparisonSource: ""
    property string rightComparisonSource: ""
    property bool compareMode: false
    property string localComparisonStatus: ""

    readonly property bool comparisonReady: leftComparisonSnapshot !== null
        && rightComparisonSnapshot !== null
    readonly property bool comparisonVisualsReady: leftComparisonVisualReady
        && rightComparisonVisualReady
    readonly property bool canSubmitComparison: comparisonReady
        && comparisonVisualsReady && comparisonBackendReady
        && compareMode && !controller.scanning && !controller.refreshing
        && !controller.comparisonBusy && !controller.decisionBusy
    readonly property bool canMutateDecision: selectedPhotoId.length > 0
        && !compareMode && !controller.scanning && !controller.refreshing
        && !controller.busy && !controller.loadingMore
        && !controller.comparisonBusy && !controller.decisionBusy
    readonly property bool canOpenSelectedPhoto: selectedPhotoId.length > 0
        && selectedRepresentationId.length > 0 && !compareMode
        && !controller.refreshing
        && !controller.busy && !controller.loadingMore
        && !controller.comparisonBusy && !controller.decisionBusy

    signal openPrecisionRequested(string photoId, string representationId,
                                  string sourcePath, string photoTitle)

    readonly property color panel: "#121519"
    readonly property color panelRaised: "#181c21"
    readonly property color border: "#2a3037"
    readonly property color textPrimary: "#edf0f2"
    readonly property color textMuted: "#8b949e"
    readonly property color accent: "#d8b36a"

    function formatLuma(value) {
        return Number(value).toFixed(3)
    }

    function formatPercent(value) {
        return (Number(value) * 100.0).toFixed(2) + "%"
    }

    function formatProxyDetail(value) {
        const number = Number(value)
        const magnitude = Math.abs(number)
        if (magnitude > 0.0 && (magnitude < 0.001 || magnitude >= 1000.0))
            return number.toExponential(3)
        return number.toFixed(4)
    }

    function concisePreprocessingVersion(value) {
        const parts = String(value).split(":")
        if (parts.length < 2)
            return value.length > 0 ? value : "—"
        return parts[0] + " · " + parts[parts.length - 1]
    }

    function chooseFolder() {
        folderDialog.open()
    }

    function selectPhoto(card) {
        selectedPhotoId = card.photoId
        selectedRepresentationId = card.representationId
        selectedVisualHandle = card.visualHandle
        selectedDecisionHeadSequence = card.decisionHeadSequence
        selectedDecisionFlag = card.decisionFlag
        selectedDecisionRating = card.decisionRating
        selectedTitle = card.title
        selectedPath = card.sourcePath
        selectedRole = card.visualRole
        selectedVisualSource = card.visualSource
        selectedWidth = card.visualWidth
        selectedHeight = card.visualHeight
        selectedHasTechnicalObservation = card.hasTechnicalObservation
        selectedTechnicalInputWidth = card.technicalInputWidth
        selectedTechnicalInputHeight = card.technicalInputHeight
        selectedTechnicalPreprocessingVersion = card.technicalPreprocessingVersion
        selectedTechnicalImplementationVersion = card.technicalImplementationVersion
        selectedMeanLuma = card.meanLuma
        selectedP01Luma = card.p01Luma
        selectedP50Luma = card.p50Luma
        selectedP99Luma = card.p99Luma
        selectedNearBlackFraction = card.nearBlackFraction
        selectedNearWhiteFraction = card.nearWhiteFraction
        selectedLaplacianVariance = card.laplacianVariance
        selectedEdgeEnergy = card.edgeEnergy
    }

    function clearSelection() {
        selectedPhotoId = ""
        selectedRepresentationId = ""
        selectedVisualHandle = ""
        selectedDecisionHeadSequence = 0
        selectedDecisionFlag = "unflagged"
        selectedDecisionRating = 0
        selectedTitle = ""
        selectedPath = ""
        selectedRole = ""
        selectedVisualSource = ""
        selectedWidth = 0
        selectedHeight = 0
        selectedHasTechnicalObservation = false
        selectedTechnicalInputWidth = 0
        selectedTechnicalInputHeight = 0
        selectedTechnicalPreprocessingVersion = ""
        selectedTechnicalImplementationVersion = ""
        selectedMeanLuma = 0.0
        selectedP01Luma = 0.0
        selectedP50Luma = 0.0
        selectedP99Luma = 0.0
        selectedNearBlackFraction = 0.0
        selectedNearWhiteFraction = 0.0
        selectedLaplacianVariance = 0.0
        selectedEdgeEnergy = 0.0
    }

    function selectedComparisonSnapshot() {
        return {
            "photoId": String(selectedPhotoId),
            "representationId": String(selectedRepresentationId),
            "visualHandle": String(selectedVisualHandle),
            "title": String(selectedTitle),
            "sourcePath": String(selectedPath),
            "visualRole": String(selectedRole),
            "visualSource": String(selectedVisualSource),
            "visualWidth": Number(selectedWidth),
            "visualHeight": Number(selectedHeight),
            "hasTechnicalObservation": Boolean(selectedHasTechnicalObservation),
            "technicalInputWidth": Number(selectedTechnicalInputWidth),
            "technicalInputHeight": Number(selectedTechnicalInputHeight),
            "technicalPreprocessingVersion": String(
                selectedTechnicalPreprocessingVersion),
            "technicalImplementationVersion": String(
                selectedTechnicalImplementationVersion),
            "meanLuma": Number(selectedMeanLuma),
            "p01Luma": Number(selectedP01Luma),
            "p50Luma": Number(selectedP50Luma),
            "p99Luma": Number(selectedP99Luma),
            "nearBlackFraction": Number(selectedNearBlackFraction),
            "nearWhiteFraction": Number(selectedNearWhiteFraction),
            "laplacianVariance": Number(selectedLaplacianVariance),
            "edgeEnergy": Number(selectedEdgeEnergy)
        }
    }

    function sameComparisonIdentity(left, right) {
        return left !== null && right !== null
            && left.photoId === right.photoId
            && left.representationId === right.representationId
            && left.visualHandle === right.visualHandle
            && left.visualSource === right.visualSource
    }

    function setSelectedAsLeft() {
        if (selectedPhotoId.length === 0 || selectedRepresentationId.length === 0
                || selectedVisualHandle.length === 0
                || selectedVisualSource.length === 0)
            return
        if (rightComparisonSnapshot !== null
                && rightComparisonSnapshot.photoId === selectedPhotoId) {
            localComparisonStatus = "A photo cannot occupy both comparison slots."
            return
        }
        const snapshot = selectedComparisonSnapshot()
        leftComparisonVisualReady = false
        comparisonBackendReady = false
        leftComparisonSnapshot = snapshot
        localComparisonStatus = "Left evidence slot updated."
    }

    function setSelectedAsRight() {
        if (selectedPhotoId.length === 0 || selectedRepresentationId.length === 0
                || selectedVisualHandle.length === 0
                || selectedVisualSource.length === 0)
            return
        if (leftComparisonSnapshot !== null
                && leftComparisonSnapshot.photoId === selectedPhotoId) {
            localComparisonStatus = "A photo cannot occupy both comparison slots."
            return
        }
        const snapshot = selectedComparisonSnapshot()
        rightComparisonVisualReady = false
        comparisonBackendReady = false
        rightComparisonSnapshot = snapshot
        localComparisonStatus = "Right evidence slot updated."
    }

    function resetPreparedComparison(cancelBackend) {
        if (cancelBackend && comparisonPresentationId.length > 0)
            controller.cancelComparison(comparisonPresentationId)
        comparisonPresentationId = ""
        leftComparisonRequestTicket = ""
        rightComparisonRequestTicket = ""
        leftComparisonSource = ""
        rightComparisonSource = ""
        leftComparisonVisualReady = false
        rightComparisonVisualReady = false
        comparisonBackendReady = false
    }

    function clearComparisonSlots(cancelBackend) {
        resetPreparedComparison(cancelBackend !== false)
        leftComparisonSnapshot = null
        rightComparisonSnapshot = null
        compareMode = false
        localComparisonStatus = ""
    }

    function enterComparison() {
        if (!comparisonReady)
            return
        const prepared = controller.prepareComparison(
            leftComparisonSnapshot.visualHandle,
            rightComparisonSnapshot.visualHandle)
        if (!prepared || String(prepared.presentationId).length === 0)
            return
        comparisonPresentationId = String(prepared.presentationId)
        leftComparisonRequestTicket = String(prepared.leftRequestTicket)
        rightComparisonRequestTicket = String(prepared.rightRequestTicket)
        leftComparisonSource = String(prepared.leftSource)
        rightComparisonSource = String(prepared.rightSource)
        leftComparisonVisualReady = false
        rightComparisonVisualReady = false
        comparisonBackendReady = false
        compareMode = true
        localComparisonStatus = ""
    }

    function exitComparison() {
        resetPreparedComparison(true)
        compareMode = false
        localComparisonStatus = ""
    }

    function refreshComparisonReadiness() {
        comparisonBackendReady = false
        if (!compareMode || !comparisonVisualsReady
                || comparisonPresentationId.length === 0)
            return
        comparisonBackendReady = controller.confirmComparisonReady(
            comparisonPresentationId,
            leftComparisonRequestTicket,
            rightComparisonRequestTicket)
    }

    function submitComparison(outcome) {
        if (!canSubmitComparison)
            return
        controller.recordComparison(comparisonPresentationId, outcome)
    }

    function openSelectedPhoto() {
        if (!canOpenSelectedPhoto)
            return
        openPrecisionRequested(selectedPhotoId, selectedRepresentationId,
                               selectedPath, selectedTitle)
    }

    function setSelectedFlag(flag) {
        if (canMutateDecision)
            controller.setPhotoFlag(selectedPhotoId, flag)
    }

    function setSelectedRating(rating) {
        if (canMutateDecision)
            controller.setPhotoRating(selectedPhotoId, rating)
    }

    function ratingGlyphs(rating) {
        let text = ""
        for (let index = 0; index < Number(rating); ++index)
            text += "★"
        return text
    }

    FolderDialog {
        id: folderDialog
        title: "Choose a photo folder"
        onAccepted: review.controller.scanFolder(selectedFolder)
    }

    Connections {
        target: review.controller
        function onItemCountChanged() {
            if (review.controller.itemCount === 0) {
                review.clearSelection()
                review.clearComparisonSlots()
            }
        }
        function onComparisonRecorded() {
            review.clearComparisonSlots(false)
            review.localComparisonStatus = ""
            grid.forceActiveFocus()
        }
        function onComparisonForgotten() {
            review.localComparisonStatus = ""
        }
        function onDecisionChanged(photoId, headSequence, flag, rating) {
            if (review.selectedPhotoId === photoId) {
                review.selectedDecisionHeadSequence = headSequence
                review.selectedDecisionFlag = flag
                review.selectedDecisionRating = rating
            }
        }
    }

    Shortcut {
        sequence: "1"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(0)
    }

    Shortcut {
        sequence: "2"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(1)
    }

    Shortcut {
        sequence: "3"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(2)
    }

    Shortcut {
        sequence: "4"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(3)
    }

    Shortcut {
        sequence: "0"
        enabled: review.visible && review.canSubmitComparison
        onActivated: review.submitComparison(4)
    }

    Shortcut {
        sequence: "Escape"
        enabled: review.visible && review.compareMode
            && !review.controller.comparisonBusy
        onActivated: review.exitComparison()
    }

    Shortcut {
        sequence: "P"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedFlag("picked")
    }

    Shortcut {
        sequence: "U"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedFlag("unflagged")
    }

    Shortcut {
        sequence: "X"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedFlag("rejected")
    }

    Shortcut {
        sequence: "0"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(0)
    }

    Shortcut {
        sequence: "1"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(1)
    }

    Shortcut {
        sequence: "2"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(2)
    }

    Shortcut {
        sequence: "3"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(3)
    }

    Shortcut {
        sequence: "4"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(4)
    }

    Shortcut {
        sequence: "5"
        enabled: review.visible && review.canMutateDecision
        onActivated: review.setSelectedRating(5)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 210
            Layout.fillHeight: true
            color: review.panel
            border.color: review.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 8

                Label {
                    text: "LIBRARY"
                    color: review.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.6
                }

                Item { Layout.preferredHeight: 6 }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    radius: 4
                    color: "#22272d"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 10
                        Label { text: "All Photos"; color: review.textPrimary }
                        Label {
                            Layout.fillWidth: true
                            text: review.controller.itemCount
                            color: review.textMuted
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    topPadding: 10
                    text: "Original files stay read-only. Review uses embedded previews first and rebuildable local proxies when needed."
                    color: review.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    lineHeight: 1.35
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: importStatusColumn.implicitHeight + 20
                    visible: review.controller.scanning
                        || (review.controller.refreshing
                            && Number(review.controller.scanProgress.scanId) > 0)
                    radius: 4
                    color: "#1a1e23"
                    border.color: "#343b43"

                    ColumnLayout {
                        id: importStatusColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 5

                        RowLayout {
                            Layout.fillWidth: true

                            Label {
                                text: review.controller.refreshing
                                        && !review.controller.scanning
                                    ? "LIBRARY REFRESH"
                                    : review.controller.scanProgress.phase === "cancelling"
                                    ? "STOPPING IMPORT" : "IMPORTING"
                                color: review.accent
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.9
                            }

                            Label {
                                Layout.fillWidth: true
                                text: review.controller.scanProgress.cataloguedFiles
                                    + " catalogued"
                                color: review.textPrimary
                                horizontalAlignment: Text.AlignRight
                                font.pixelSize: 9
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: review.controller.scanProgress.supportedFiles
                                + " supported · "
                                + (review.controller.scanning
                                    ? review.controller.scanProgress.decodeQueued
                                        + " preview checks queued"
                                    : review.controller.scanProgress.decodeCompleted + "/"
                                        + review.controller.scanProgress.decodeQueued
                                        + " preview checks completed")
                                + " · "
                                + review.controller.scanProgress.issueCount
                                + " filesystem issues"
                            color: review.textMuted
                            elide: Text.ElideRight
                            font.pixelSize: 9
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !review.controller.scanning
                            text: review.controller.scanProgress.decodeHardFailures
                                + " decode failures · "
                                + review.controller.scanProgress.previewFailures
                                + " preview failures · "
                                + review.controller.scanProgress.decodeCancelled
                                + " cancelled"
                            color: review.textMuted
                            elide: Text.ElideRight
                            font.pixelSize: 9
                        }

                        ProgressBar {
                            Layout.fillWidth: true
                            indeterminate: true
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    Layout.topMargin: 8
                    color: review.border
                }

                Label {
                    text: "COMPARE EVIDENCE"
                    color: review.textMuted
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.0
                }

                Label {
                    Layout.fillWidth: true
                    text: review.controller.sessionEvidenceCount
                        + " active this session"
                    color: review.textPrimary
                    font.pixelSize: 11
                }

                Button {
                    id: undoComparisonButton
                    Layout.fillWidth: true
                    enabled: review.controller.canUndoComparison
                        && !review.controller.comparisonBusy
                        && !review.controller.decisionBusy
                        && !review.controller.scanning
                        && !review.controller.refreshing
                        && !review.controller.busy
                        && !review.controller.loadingMore
                    text: "FORGET LAST"
                    onClicked: review.controller.undoLastComparison()
                }

                Label {
                    Layout.fillWidth: true
                    text: "Forget appends a fact; it does not delete the original evidence event."
                    color: "#66717c"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 9
                    lineHeight: 1.3
                }

                Label {
                    Layout.fillWidth: true
                    visible: !review.compareMode
                        && (review.controller.comparisonStatusText.length > 0
                            || review.localComparisonStatus.length > 0)
                    text: review.localComparisonStatus.length > 0
                        ? review.localComparisonStatus
                        : review.controller.comparisonStatusText
                    color: review.controller.comparisonBusy
                        ? review.accent : review.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: 9
                }

                Item { Layout.fillHeight: true }

                Label {
                    text: "LOCAL · MACOS"
                    color: "#64707b"
                    font.pixelSize: 9
                    font.letterSpacing: 1.2
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#0c0e10"

            GridView {
                id: grid
                objectName: "reviewGrid"

                function maybeLoadMore() {
                    if (review.controller.hasMore
                            && !review.controller.scanning
                            && !review.controller.refreshing
                            && !review.controller.busy
                            && !review.controller.loadingMore
                            && contentY + height >= contentHeight - cellHeight * 2) {
                        review.controller.loadMore()
                    }
                }

                anchors.fill: parent
                anchors.margins: 18
                clip: true
                visible: !review.compareMode
                enabled: visible
                focus: visible
                model: review.controller.model
                cellWidth: Math.max(220, Math.floor(width / Math.max(1, Math.floor(width / 270))))
                cellHeight: cellWidth * 0.78
                currentIndex: -1
                onContentYChanged: maybeLoadMore()
                onHeightChanged: Qt.callLater(maybeLoadMore)
                onCountChanged: {
                    if (count === 0)
                        currentIndex = -1
                    else if (currentIndex < 0)
                        currentIndex = 0
                    Qt.callLater(maybeLoadMore)
                }
                onCurrentItemChanged: {
                    if (currentItem)
                        review.selectPhoto(currentItem)
                }

                delegate: Item {
                    id: card
                    width: grid.cellWidth
                    height: grid.cellHeight

                    required property int index
                    required property string photoId
                    required property string representationId
                    required property string visualHandle
                    required property string title
                    required property string sourcePath
                    required property string visualRole
                    required property string visualError
                    required property int visualWidth
                    required property int visualHeight
                    required property string visualSource
                    required property var decisionHeadSequence
                    required property string decisionFlag
                    required property int decisionRating
                    required property bool hasTechnicalObservation
                    required property int technicalInputWidth
                    required property int technicalInputHeight
                    required property string technicalPreprocessingVersion
                    required property string technicalImplementationVersion
                    required property real meanLuma
                    required property real p01Luma
                    required property real p50Luma
                    required property real p99Luma
                    required property real nearBlackFraction
                    required property real nearWhiteFraction
                    required property real laplacianVariance
                    required property real edgeEnergy

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 5
                        radius: 5
                        color: review.panelRaised
                        border.width: grid.currentIndex === card.index ? 2 : 1
                        border.color: grid.currentIndex === card.index ? review.accent : review.border

                        Image {
                            id: thumbnail
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: caption.top
                            source: card.visualSource
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: true
                            sourceSize.width: 640
                            sourceSize.height: 480
                        }

                        Rectangle {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.topMargin: 12
                            anchors.rightMargin: 12
                            width: decisionFlagLabel.implicitWidth + 14
                            height: 22
                            radius: 3
                            visible: card.decisionFlag !== "unflagged"
                            color: card.decisionFlag === "picked"
                                ? "#214030" : "#492a28"
                            border.color: card.decisionFlag === "picked"
                                ? "#609677" : "#a4645d"

                            Label {
                                id: decisionFlagLabel
                                anchors.centerIn: parent
                                text: card.decisionFlag === "picked" ? "PICK" : "REJECT"
                                color: card.decisionFlag === "picked"
                                    ? "#a7d2b6" : "#e2aaa3"
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.8
                            }
                        }

                        Rectangle {
                            anchors.fill: thumbnail
                            visible: card.visualSource.length === 0
                            color: "#20252b"

                            Column {
                                anchors.centerIn: parent
                                spacing: 8
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "RAW"
                                    color: "#727d88"
                                    font.pixelSize: 20
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 2
                                }
                                Label {
                                    text: card.visualError.length > 0 ? "PREVIEW PENDING" : "NO VISUAL"
                                    color: review.textMuted
                                    font.pixelSize: 9
                                }
                            }
                        }

                        Rectangle {
                            id: caption
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 52
                            color: "#e615181c"

                            Column {
                                anchors.left: parent.left
                                anchors.right: cardMetadata.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 10
                                spacing: 3
                                Label {
                                    width: parent.width
                                    text: card.title
                                    color: review.textPrimary
                                    elide: Text.ElideRight
                                    font.pixelSize: 11
                                }
                                Label {
                                    text: card.visualWidth > 0
                                        ? card.visualWidth + " × " + card.visualHeight
                                        : "awaiting cache"
                                    color: review.textMuted
                                    font.pixelSize: 9
                                }
                            }

                            Column {
                                id: cardMetadata
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3

                                Label {
                                    anchors.right: parent.right
                                    text: card.visualRole.length > 0
                                        ? card.visualRole.toUpperCase() : "RAW"
                                    color: card.visualRole === "embedded"
                                        ? "#9fc7a7" : review.accent
                                    font.pixelSize: 8
                                    font.weight: Font.Bold
                                    font.letterSpacing: 0.8
                                }

                                Label {
                                    anchors.right: parent.right
                                    visible: card.decisionRating > 0
                                    text: review.ratingGlyphs(card.decisionRating)
                                    color: review.accent
                                    font.pixelSize: 9
                                    font.letterSpacing: 0.4
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                grid.currentIndex = card.index
                                review.selectPhoto(card)
                            }
                            onDoubleClicked: {
                                grid.currentIndex = card.index
                                review.selectPhoto(card)
                                review.openSelectedPhoto()
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: Math.min(420, parent.width - 60)
                    visible: grid.count === 0 && !review.controller.busy
                    text: review.controller.scanning
                        ? "Searching the folder for supported photos…\nNew RAW files will appear here as they are catalogued."
                        : review.controller.scanProgress.phase === "failed"
                        ? "Import stopped, and no RAW files are currently visible.\nAlready catalogued files remain safely stored."
                        : "Add a folder to the local Library.\nShadow will use embedded previews first and generate a local proxy only when needed."
                    color: review.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    lineHeight: 1.4
                }

                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 18
                    visible: review.controller.loadingMore
                    running: visible
                    width: 34
                    height: 34
                }
            }

            Item {
                id: comparisonView
                anchors.fill: parent
                anchors.margins: 18
                visible: review.compareMode

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                text: "COMPARE EVIDENCE"
                                color: review.textPrimary
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1.1
                            }

                            Label {
                                Layout.fillWidth: true
                                text: "A local preference event, not a rank or an AI score."
                                color: review.textMuted
                                font.pixelSize: 10
                            }
                        }

                        Button {
                            text: "EXIT COMPARE"
                            enabled: !review.controller.comparisonBusy
                            onClicked: review.exitComparison()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 12

                        Repeater {
                            model: [review.leftComparisonSnapshot,
                                    review.rightComparisonSnapshot]

                            delegate: Rectangle {
                                id: comparisonCard

                                required property int index
                                required property var modelData

                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredWidth: 1
                                radius: 5
                                color: review.panelRaised
                                border.color: review.border

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 8

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Label {
                                            text: comparisonCard.index === 0 ? "LEFT · A" : "RIGHT · B"
                                            color: review.accent
                                            font.pixelSize: 9
                                            font.weight: Font.Bold
                                            font.letterSpacing: 1.0
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: comparisonCard.modelData
                                                ? comparisonCard.modelData.title : ""
                                            color: review.textPrimary
                                            horizontalAlignment: Text.AlignRight
                                            elide: Text.ElideMiddle
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Layout.minimumHeight: 180
                                        color: "#08090a"
                                        border.color: "#23282e"

                                        Image {
                                            id: comparisonImage
                                            anchors.fill: parent
                                            anchors.margins: 1
                                            source: comparisonCard.index === 0
                                                ? review.leftComparisonSource
                                                : review.rightComparisonSource
                                            fillMode: Image.PreserveAspectFit
                                            asynchronous: true
                                            cache: false
                                            sourceSize.width: 1280
                                            sourceSize.height: 960
                                            onStatusChanged: {
                                                const current = comparisonCard.index === 0
                                                    ? review.leftComparisonSnapshot
                                                    : review.rightComparisonSnapshot
                                                if (!review.sameComparisonIdentity(
                                                        current,
                                                        comparisonCard.modelData)
                                                        || String(comparisonImage.source)
                                                            !== (comparisonCard.index === 0
                                                                ? review.leftComparisonSource
                                                                : review.rightComparisonSource))
                                                    return
                                                if (comparisonCard.index === 0)
                                                    review.leftComparisonVisualReady = status === Image.Ready
                                                else
                                                    review.rightComparisonVisualReady = status === Image.Ready
                                                Qt.callLater(review.refreshComparisonReadiness)
                                            }
                                        }

                                        Label {
                                            anchors.centerIn: parent
                                            visible: comparisonImage.status !== Image.Ready
                                            text: comparisonImage.status === Image.Error
                                                ? "VISUAL LOAD FAILED"
                                                : "LOADING VERIFIED VISUAL…"
                                            color: comparisonImage.status === Image.Error
                                                ? "#d28e82" : review.textMuted
                                            font.pixelSize: 9
                                            font.weight: Font.DemiBold
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Label {
                                            text: comparisonCard.modelData
                                                && comparisonCard.modelData.visualRole.length > 0
                                                ? comparisonCard.modelData.visualRole.toUpperCase()
                                                : "DISPLAY PROXY"
                                            color: review.textMuted
                                            font.pixelSize: 8
                                            font.weight: Font.Bold
                                            font.letterSpacing: 0.8
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: comparisonCard.modelData
                                                && comparisonCard.modelData.visualWidth > 0
                                                ? comparisonCard.modelData.visualWidth + " × "
                                                    + comparisonCard.modelData.visualHeight
                                                : ""
                                            color: review.textMuted
                                            horizontalAlignment: Text.AlignRight
                                            font.pixelSize: 9
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: review.border
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: "TECHNICAL · DISPLAY PROXY"
                                        color: review.textMuted
                                        font.pixelSize: 9
                                        font.weight: Font.DemiBold
                                        font.letterSpacing: 0.8
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && !comparisonCard.modelData.hasTechnicalObservation
                                        text: "No observation recorded — visual comparison is still available."
                                        color: "#64707b"
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 9
                                    }

                                    GridLayout {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        columns: 4
                                        rowSpacing: 4
                                        columnSpacing: 8

                                        Label { text: "MEAN"; color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.meanLuma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: "P50"; color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.p50Luma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: "P01"; color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.p01Luma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: "P99"; color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatLuma(comparisonCard.modelData.p99Luma) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: "BLACK"; color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatPercent(
                                                    comparisonCard.modelData.nearBlackFraction) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: "WHITE"; color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatPercent(
                                                    comparisonCard.modelData.nearWhiteFraction) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: "LAPL."; color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatProxyDetail(
                                                    comparisonCard.modelData.laplacianVariance) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                        Label { text: "EDGE"; color: review.textMuted; font.pixelSize: 8 }
                                        Label {
                                            text: comparisonCard.modelData
                                                ? review.formatProxyDetail(
                                                    comparisonCard.modelData.edgeEnergy) : "—"
                                            color: review.textPrimary
                                            font.pixelSize: 9
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        text: comparisonCard.modelData
                                            ? "INPUT  " + comparisonCard.modelData.technicalInputWidth
                                                + " × " + comparisonCard.modelData.technicalInputHeight
                                            : ""
                                        color: "#66717c"
                                        font.pixelSize: 8
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        text: comparisonCard.modelData
                                            ? "PIPELINE  " + review.concisePreprocessingVersion(
                                                comparisonCard.modelData.technicalPreprocessingVersion)
                                            : ""
                                        color: "#66717c"
                                        elide: Text.ElideRight
                                        font.pixelSize: 8
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: comparisonCard.modelData
                                            && comparisonCard.modelData.hasTechnicalObservation
                                        text: comparisonCard.modelData
                                            ? "ANALYZER  "
                                                + comparisonCard.modelData.technicalImplementationVersion
                                            : ""
                                        color: "#66717c"
                                        elide: Text.ElideRight
                                        font.pixelSize: 8
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: review.comparisonBackendReady
                            ? "EXACT ARTIFACTS + DECODED RGBA FRAMES VERIFIED"
                            : "WAITING FOR BOTH EXACT COMPARE FRAME RECEIPTS"
                        color: review.comparisonBackendReady
                            ? "#78a889" : "#69737d"
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.7
                    }

                    Label {
                        Layout.fillWidth: true
                        text: "Feature models and ranking are not enabled. Technical facts come from each display proxy; different proxy upstreams may not be directly comparable."
                        color: "#8f7a54"
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: 9
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Repeater {
                            model: ListModel {
                                ListElement { actionText: "1 · LEFT PREFERRED"; outcomeValue: 0 }
                                ListElement { actionText: "2 · RIGHT PREFERRED"; outcomeValue: 1 }
                                ListElement { actionText: "3 · KEEP BOTH"; outcomeValue: 2 }
                                ListElement { actionText: "4 · KEEP NEITHER"; outcomeValue: 3 }
                                ListElement { actionText: "0 · CANNOT COMPARE"; outcomeValue: 4 }
                            }

                            delegate: Button {
                                id: outcomeButton

                                required property string actionText
                                required property int outcomeValue

                                Layout.fillWidth: true
                                Layout.preferredHeight: 36
                                enabled: review.canSubmitComparison
                                text: actionText
                                onClicked: review.submitComparison(outcomeValue)

                                background: Rectangle {
                                    radius: 4
                                    color: outcomeButton.enabled
                                        ? (outcomeButton.down ? "#3a3429" : "#272b30")
                                        : "#20242a"
                                    border.color: outcomeButton.enabled
                                        ? review.accent : review.border
                                }

                                contentItem: Label {
                                    text: outcomeButton.text
                                    color: outcomeButton.enabled
                                        ? review.textPrimary : "#606a74"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    font.pixelSize: 9
                                    font.weight: Font.DemiBold
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        BusyIndicator {
                            visible: review.controller.comparisonBusy
                            running: visible
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 22
                        }

                        Label {
                            Layout.fillWidth: true
                            text: review.localComparisonStatus.length > 0
                                ? review.localComparisonStatus
                                : review.controller.comparisonStatusText
                            color: review.controller.comparisonBusy
                                ? review.accent : review.textMuted
                            elide: Text.ElideRight
                            font.pixelSize: 9
                        }
                    }
                }
            }

            Rectangle {
                anchors.fill: parent
                visible: review.controller.busy
                color: "#b00c0e10"

                Column {
                    anchors.centerIn: parent
                    spacing: 14
                    BusyIndicator {
                        anchors.horizontalCenter: parent.horizontalCenter
                        running: true
                    }
                    Label {
                        text: review.controller.scanning
                            ? "Finding the first photos" : "Loading local Library"
                        color: review.textPrimary
                        font.pixelSize: 14
                    }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 278
            Layout.fillHeight: true
            color: review.panel
            border.color: review.border

            ScrollView {
                id: photoInspectorScroll

                anchors.fill: parent
                anchors.margins: 18
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: photoInspectorScroll.availableWidth
                    spacing: 10

                Label {
                    text: "PHOTO"
                    color: review.textMuted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.6
                }

                Label {
                    Layout.fillWidth: true
                    text: review.selectedTitle.length > 0 ? review.selectedTitle : "Nothing selected"
                    color: review.textPrimary
                    font.pixelSize: 16
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    text: review.selectedPath
                    color: review.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 3
                    elide: Text.ElideMiddle
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: review.border
                }

                GridLayout {
                    columns: 2
                    rowSpacing: 8
                    columnSpacing: 12

                    Label { text: "VISUAL"; color: review.textMuted; font.pixelSize: 9 }
                    Label {
                        text: review.selectedRole.length > 0
                            ? review.selectedRole.toUpperCase()
                            : "PENDING"
                        color: review.textPrimary
                        font.pixelSize: 10
                    }
                    Label { text: "SIZE"; color: review.textMuted; font.pixelSize: 9 }
                    Label {
                        text: review.selectedWidth > 0
                            ? review.selectedWidth + " × " + review.selectedHeight
                            : "—"
                        color: review.textPrimary
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: review.border
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            text: "DECISION LEDGER"
                            color: review.textMuted
                            font.pixelSize: 9
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.0
                        }

                        BusyIndicator {
                            visible: review.controller.decisionBusy
                            running: visible
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                        }

                        Item { Layout.fillWidth: true }

                        Button {
                            enabled: !review.compareMode && !review.controller.scanning
                                && !review.controller.refreshing
                                && !review.controller.busy
                                && !review.controller.loadingMore
                                && !review.controller.comparisonBusy
                                && review.controller.canUndoDecision
                            text: "UNDO"
                            onClicked: review.controller.undoLastDecision()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 5

                        Repeater {
                            model: ListModel {
                                ListElement { flagText: "U · NONE"; flagValue: "unflagged" }
                                ListElement { flagText: "P · PICK"; flagValue: "picked" }
                                ListElement { flagText: "X · REJECT"; flagValue: "rejected" }
                            }

                            delegate: Button {
                                id: flagButton

                                required property string flagText
                                required property string flagValue

                                Layout.fillWidth: true
                                Layout.preferredHeight: 30
                                enabled: review.canMutateDecision
                                text: flagText
                                onClicked: review.setSelectedFlag(flagValue)

                                background: Rectangle {
                                    radius: 3
                                    color: review.selectedDecisionFlag === flagButton.flagValue
                                        ? (flagButton.flagValue === "picked"
                                            ? "#214030"
                                            : flagButton.flagValue === "rejected"
                                                ? "#492a28" : "#343a41")
                                        : "#20242a"
                                    border.color: review.selectedDecisionFlag === flagButton.flagValue
                                        ? review.accent : review.border
                                }

                                contentItem: Label {
                                    text: flagButton.text
                                    color: flagButton.enabled
                                        ? review.textPrimary : "#606a74"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    font.pixelSize: 8
                                    font.weight: Font.DemiBold
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Repeater {
                            model: 6

                            delegate: Button {
                                id: ratingButton

                                required property int index

                                Layout.fillWidth: true
                                Layout.preferredHeight: 28
                                enabled: review.canMutateDecision
                                text: index === 0 ? "0" : "★"
                                onClicked: review.setSelectedRating(index)

                                background: Rectangle {
                                    radius: 3
                                    color: review.selectedDecisionRating === ratingButton.index
                                        ? "#3b3324" : "#20242a"
                                    border.color: review.selectedDecisionRating === ratingButton.index
                                        ? review.accent : review.border
                                }

                                contentItem: Label {
                                    text: ratingButton.text
                                    color: ratingButton.index > 0
                                        && ratingButton.index <= review.selectedDecisionRating
                                        ? review.accent
                                        : ratingButton.enabled ? review.textPrimary : "#606a74"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: review.controller.decisionStatusText
                        color: review.controller.decisionBusy
                            ? review.accent : "#66717c"
                        elide: Text.ElideRight
                        font.pixelSize: 8
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    visible: review.selectedPhotoId.length > 0
                    color: review.border
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: review.selectedPhotoId.length > 0
                    spacing: 5

                    Label {
                        text: "TECHNICAL · DISPLAY PROXY"
                        color: review.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.0
                    }

                    Label {
                        visible: !review.selectedHasTechnicalObservation
                        text: "NOT AVAILABLE"
                        color: "#64707b"
                        font.pixelSize: 10
                        font.weight: Font.Medium
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        visible: review.selectedHasTechnicalObservation
                        columns: 2
                        rowSpacing: 5
                        columnSpacing: 10

                        Label { text: "LUMA MEAN"; color: review.textMuted; font.pixelSize: 9 }
                        Label {
                            text: review.formatLuma(review.selectedMeanLuma)
                            color: review.textPrimary
                            font.pixelSize: 10
                        }
                        Label { text: "LUMA P01"; color: review.textMuted; font.pixelSize: 9 }
                        Label {
                            text: review.formatLuma(review.selectedP01Luma)
                            color: review.textPrimary
                            font.pixelSize: 10
                        }
                        Label { text: "LUMA P50"; color: review.textMuted; font.pixelSize: 9 }
                        Label {
                            text: review.formatLuma(review.selectedP50Luma)
                            color: review.textPrimary
                            font.pixelSize: 10
                        }
                        Label { text: "LUMA P99"; color: review.textMuted; font.pixelSize: 9 }
                        Label {
                            text: review.formatLuma(review.selectedP99Luma)
                            color: review.textPrimary
                            font.pixelSize: 10
                        }
                        Label { text: "NEAR BLACK"; color: review.textMuted; font.pixelSize: 9 }
                        Label {
                            text: review.formatPercent(review.selectedNearBlackFraction)
                            color: review.textPrimary
                            font.pixelSize: 10
                        }
                        Label { text: "NEAR WHITE"; color: review.textMuted; font.pixelSize: 9 }
                        Label {
                            text: review.formatPercent(review.selectedNearWhiteFraction)
                            color: review.textPrimary
                            font.pixelSize: 10
                        }
                        Label {
                            text: "LAPLACIAN · PROXY"
                            color: review.textMuted
                            font.pixelSize: 9
                        }
                        Label {
                            text: review.formatProxyDetail(review.selectedLaplacianVariance)
                            color: review.textPrimary
                            font.pixelSize: 10
                        }
                        Label {
                            text: "EDGE ENERGY · PROXY"
                            color: review.textMuted
                            font.pixelSize: 9
                        }
                        Label {
                            text: review.formatProxyDetail(review.selectedEdgeEnergy)
                            color: review.textPrimary
                            font.pixelSize: 10
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: review.selectedHasTechnicalObservation
                        text: "INPUT  " + review.selectedTechnicalInputWidth + " × "
                            + review.selectedTechnicalInputHeight
                        color: "#66717c"
                        font.pixelSize: 9
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: review.selectedHasTechnicalObservation
                        text: "PIPELINE  " + review.concisePreprocessingVersion(
                            review.selectedTechnicalPreprocessingVersion)
                        color: "#66717c"
                        font.pixelSize: 8
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: review.selectedHasTechnicalObservation
                        text: "ANALYZER  "
                            + review.selectedTechnicalImplementationVersion
                        color: "#66717c"
                        font.pixelSize: 8
                        elide: Text.ElideRight
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    visible: review.selectedPhotoId.length > 0
                    color: review.border
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    Label {
                        text: "COMPARE SLOTS"
                        color: review.textMuted
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.0
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 7

                        Button {
                            id: setLeftButton
                            Layout.fillWidth: true
                            enabled: !review.compareMode
                                && !review.controller.comparisonBusy
                                && !review.controller.decisionBusy
                                && !review.controller.scanning
                                && !review.controller.refreshing
                                && !review.controller.busy
                                && !review.controller.loadingMore
                                && review.selectedPhotoId.length > 0
                                && review.selectedRepresentationId.length > 0
                                && review.selectedVisualHandle.length > 0
                                && review.selectedVisualSource.length > 0
                                && (review.rightComparisonSnapshot === null
                                    || review.rightComparisonSnapshot.photoId
                                        !== review.selectedPhotoId)
                            text: "SET LEFT · A"
                            onClicked: review.setSelectedAsLeft()
                        }

                        Button {
                            id: setRightButton
                            Layout.fillWidth: true
                            enabled: !review.compareMode
                                && !review.controller.comparisonBusy
                                && !review.controller.decisionBusy
                                && !review.controller.scanning
                                && !review.controller.refreshing
                                && !review.controller.busy
                                && !review.controller.loadingMore
                                && review.selectedPhotoId.length > 0
                                && review.selectedRepresentationId.length > 0
                                && review.selectedVisualHandle.length > 0
                                && review.selectedVisualSource.length > 0
                                && (review.leftComparisonSnapshot === null
                                    || review.leftComparisonSnapshot.photoId
                                        !== review.selectedPhotoId)
                            text: "SET RIGHT · B"
                            onClicked: review.setSelectedAsRight()
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: "A  " + (review.leftComparisonSnapshot
                            ? review.leftComparisonSnapshot.title : "Not set")
                        color: review.leftComparisonSnapshot
                            ? review.textPrimary : review.textMuted
                        elide: Text.ElideMiddle
                        font.pixelSize: 9
                    }

                    Label {
                        Layout.fillWidth: true
                        text: "B  " + (review.rightComparisonSnapshot
                            ? review.rightComparisonSnapshot.title : "Not set")
                        color: review.rightComparisonSnapshot
                            ? review.textPrimary : review.textMuted
                        elide: Text.ElideMiddle
                        font.pixelSize: 9
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: review.selectedPhotoId.length > 0
                            && review.selectedVisualSource.length === 0
                        text: "A display visual is required for comparison."
                        color: "#8f7a54"
                        wrapMode: Text.WordWrap
                        font.pixelSize: 9
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 7

                        Button {
                            Layout.fillWidth: true
                            enabled: review.comparisonReady
                                && !review.compareMode
                                && !review.controller.comparisonBusy
                                && !review.controller.decisionBusy
                                && !review.controller.scanning
                                && !review.controller.refreshing
                                && !review.controller.busy
                                && !review.controller.loadingMore
                            text: "COMPARE A / B"
                            onClicked: review.enterComparison()
                        }

                        Button {
                            enabled: !review.controller.comparisonBusy
                                && !review.controller.decisionBusy
                                && !review.controller.scanning
                                && !review.controller.refreshing
                                && !review.controller.busy
                                && !review.controller.loadingMore
                                && (review.leftComparisonSnapshot !== null
                                    || review.rightComparisonSnapshot !== null)
                            text: "CLEAR"
                            onClicked: review.clearComparisonSlots()
                        }
                    }
                }

                Button {
                    id: openButton
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    enabled: review.canOpenSelectedPhoto
                    text: "OPEN IN PRECISION"
                    onClicked: review.openSelectedPhoto()

                    background: Rectangle {
                        radius: 4
                        color: openButton.enabled
                            ? (openButton.down ? "#b9914e" : review.accent)
                            : "#252a30"
                    }
                    contentItem: Label {
                        text: openButton.text
                        color: openButton.enabled ? "#17130d" : "#606a74"
                        font.pixelSize: 10
                        font.weight: Font.Bold
                        font.letterSpacing: 0.6
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Item { Layout.preferredHeight: 4 }

                Label {
                    Layout.fillWidth: true
                    text: "Double-click a photo to enter its non-destructive Precision workspace."
                    color: "#66717c"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    lineHeight: 1.35
                }
                }
            }
        }
    }
}
