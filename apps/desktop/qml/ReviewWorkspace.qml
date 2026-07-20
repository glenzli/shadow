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
    property bool compareMode: false
    property string localComparisonStatus: ""

    readonly property bool comparisonReady: leftComparisonSnapshot !== null
        && rightComparisonSnapshot !== null
    readonly property bool comparisonVisualsReady: leftComparisonVisualReady
        && rightComparisonVisualReady
    readonly property bool canSubmitComparison: comparisonReady
        && comparisonVisualsReady && compareMode && !controller.comparisonBusy

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
            && left.visualSource === right.visualSource
    }

    function setSelectedAsLeft() {
        if (selectedPhotoId.length === 0 || selectedRepresentationId.length === 0
                || selectedVisualSource.length === 0)
            return
        if (rightComparisonSnapshot !== null
                && rightComparisonSnapshot.photoId === selectedPhotoId) {
            localComparisonStatus = "A photo cannot occupy both comparison slots."
            return
        }
        const snapshot = selectedComparisonSnapshot()
        leftComparisonVisualReady = sameComparisonIdentity(
            leftComparisonSnapshot, snapshot)
            && leftComparisonVisualReady
        leftComparisonSnapshot = snapshot
        localComparisonStatus = "Left evidence slot updated."
    }

    function setSelectedAsRight() {
        if (selectedPhotoId.length === 0 || selectedRepresentationId.length === 0
                || selectedVisualSource.length === 0)
            return
        if (leftComparisonSnapshot !== null
                && leftComparisonSnapshot.photoId === selectedPhotoId) {
            localComparisonStatus = "A photo cannot occupy both comparison slots."
            return
        }
        const snapshot = selectedComparisonSnapshot()
        rightComparisonVisualReady = sameComparisonIdentity(
            rightComparisonSnapshot, snapshot)
            && rightComparisonVisualReady
        rightComparisonSnapshot = snapshot
        localComparisonStatus = "Right evidence slot updated."
    }

    function clearComparisonSlots() {
        leftComparisonSnapshot = null
        rightComparisonSnapshot = null
        leftComparisonVisualReady = false
        rightComparisonVisualReady = false
        compareMode = false
        localComparisonStatus = ""
    }

    function enterComparison() {
        if (comparisonReady) {
            compareMode = true
            localComparisonStatus = ""
        }
    }

    function submitComparison(outcome) {
        if (!canSubmitComparison)
            return
        controller.recordComparison(
            leftComparisonSnapshot.photoId,
            leftComparisonSnapshot.representationId,
            rightComparisonSnapshot.photoId,
            rightComparisonSnapshot.representationId,
            outcome)
    }

    function openSelectedPhoto() {
        if (selectedPhotoId.length > 0 && selectedRepresentationId.length > 0) {
            openPrecisionRequested(selectedPhotoId, selectedRepresentationId,
                                   selectedPath, selectedTitle)
        }
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
            review.clearComparisonSlots()
            review.localComparisonStatus = ""
            grid.forceActiveFocus()
        }
        function onComparisonForgotten() {
            review.localComparisonStatus = ""
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
        onActivated: review.compareMode = false
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

                function maybeLoadMore() {
                    if (review.controller.hasMore
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
                    required property string title
                    required property string sourcePath
                    required property string visualRole
                    required property string visualError
                    required property int visualWidth
                    required property int visualHeight
                    required property string visualSource
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
                                anchors.right: badge.left
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

                            Label {
                                id: badge
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.visualRole.length > 0 ? card.visualRole.toUpperCase() : "RAW"
                                color: card.visualRole === "embedded" ? "#9fc7a7" : review.accent
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                font.letterSpacing: 0.8
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
                                review.openPrecisionRequested(card.photoId, card.representationId,
                                                              card.sourcePath, card.title)
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: Math.min(420, parent.width - 60)
                    visible: grid.count === 0 && !review.controller.busy
                    text: "Choose a folder to scan RAW files.\nShadow will use embedded previews first and generate a local proxy only when needed."
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
                            onClicked: review.compareMode = false
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
                                            source: comparisonCard.modelData
                                                ? comparisonCard.modelData.visualSource : ""
                                            fillMode: Image.PreserveAspectFit
                                            asynchronous: true
                                            cache: true
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
                                                            !== current.visualSource)
                                                    return
                                                if (comparisonCard.index === 0)
                                                    review.leftComparisonVisualReady = status === Image.Ready
                                                else
                                                    review.rightComparisonVisualReady = status === Image.Ready
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
                        text: "Building Review previews"
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

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
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
                                && review.selectedPhotoId.length > 0
                                && review.selectedRepresentationId.length > 0
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
                                && review.selectedPhotoId.length > 0
                                && review.selectedRepresentationId.length > 0
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
                            text: "COMPARE A / B"
                            onClicked: review.enterComparison()
                        }

                        Button {
                            enabled: !review.controller.comparisonBusy
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
                    enabled: review.selectedPhotoId.length > 0
                        && review.selectedRepresentationId.length > 0
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

                Item { Layout.fillHeight: true }

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
