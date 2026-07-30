pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One creation transaction chooses a selector and its node destination. The
// caller supplies only the entry-point default; this popup never chains node
// creation and mask mutation in QML.
Popup {
    id: menu

    required property var editor

    property int destination: 1
    property string openedPhotoId: ""
    property string openedRepresentationId: ""
    property string openedGradeNodeId: ""
    readonly property int currentNodeDestination: 0
    readonly property int newNodeDestination: 1
    readonly property int currentMaskKind: Number(menu.editor.selectedLocalMask.kind || 0)
    readonly property bool currentNodeAvailable: menu.editor.active && menu.editor.hasSelectedGradeNode && menu.editor.gradeNodeEnabled && menu.currentMaskKind === 0 && !menu.editor.stateBusy
    readonly property bool newNodeAvailable: menu.editor.canAddGradeNode && !menu.editor.stateBusy

    signal maskCreated
    signal aiMaskRequested
    signal editExistingRequested

    function selectDestination(preferredDestination) {
        destination = preferredDestination;
        if (destination === currentNodeDestination && !currentNodeAvailable) {
            destination = newNodeAvailable ? newNodeDestination : currentNodeDestination;
        } else if (destination === newNodeDestination && !newNodeAvailable && currentNodeAvailable) {
            destination = currentNodeDestination;
        }
    }

    function openFor(anchorItem, preferredDestination) {
        selectDestination(preferredDestination);
        openedPhotoId = menu.editor.photoId;
        openedRepresentationId = menu.editor.representationId;
        openedGradeNodeId = menu.editor.selectedGradeNodeId;
        const point = anchorItem.mapToItem(menu.parent, anchorItem.width - menu.width, anchorItem.height + 6);
        menu.x = Math.max(8, Math.min(point.x, menu.parent.width - menu.width - 8));
        menu.y = Math.max(8, Math.min(point.y, menu.parent.height - menu.height - 8));
        menu.open();
    }

    function closeIfTargetChanged() {
        if (!menu.visible)
            return;
        if (!menu.editor.active || menu.editor.photoId !== menu.openedPhotoId || menu.editor.representationId !== menu.openedRepresentationId || menu.editor.selectedGradeNodeId !== menu.openedGradeNodeId) {
            menu.close();
        }
    }

    function createMask(kind) {
        const available = destination === currentNodeDestination ? currentNodeAvailable : newNodeAvailable;
        if (!available)
            return;
        if (menu.editor.createLocalMask(kind, destination)) {
            menu.close();
            menu.maskCreated();
        }
    }

    function startAiMask() {
        if (!currentNodeAvailable)
            return;
        if (menu.editor.beginAiMaskPrompt()) {
            menu.close();
            menu.aiMaskRequested();
        }
    }

    parent: Overlay.overlay
    width: 268
    padding: 10
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        radius: 9
        color: Theme.panelRaised
        border.width: 1
        border.color: Theme.borderStrong
    }

    Connections {
        target: menu.editor

        function onActiveChanged() {
            menu.closeIfTargetChanged();
        }

        function onSourceIdentityChanged() {
            menu.closeIfTargetChanged();
        }

        function onSelectedGradeNodeChanged() {
            menu.closeIfTargetChanged();
        }
    }

    component MaskAction: Button {
        id: action

        required property url iconSource
        required property int maskKind

        Layout.fillWidth: true
        implicitHeight: 40
        leftPadding: 10
        rightPadding: 10
        hoverEnabled: enabled
        Accessible.name: text

        background: Rectangle {
            radius: 6
            color: action.down ? Theme.buttonGhostPressed : action.hovered ? Theme.buttonGhostHover : Theme.transparent
        }

        contentItem: RowLayout {
            spacing: 10
            ShadowIcon {
                source: action.iconSource
                color: action.enabled ? Theme.textSecondary : Theme.textDisabled
                size: 19
            }
            Label {
                Layout.fillWidth: true
                text: action.text
                color: action.enabled ? Theme.textPrimary : Theme.textDisabled
                font.pixelSize: 11
                font.weight: Font.Medium
            }
        }

        onClicked: menu.createMask(maskKind)
    }

    contentItem: ColumnLayout {
        spacing: 4

        Label {
            Layout.fillWidth: true
            leftPadding: 8
            rightPadding: 8
            topPadding: 4
            bottomPadding: 4
            text: qsTr("CREATE MASK")
            color: Theme.textMuted
            font.pixelSize: 9
            font.weight: Font.DemiBold
            font.letterSpacing: 0.7
        }

        Button {
            id: editCurrentMaskButton
            Layout.fillWidth: true
            implicitHeight: visible ? 40 : 0
            visible: menu.currentMaskKind !== 0
            leftPadding: 9
            rightPadding: 9
            hoverEnabled: enabled
            text: qsTr("Edit current node mask")
            Accessible.name: text
            background: Rectangle {
                radius: 6
                color: editCurrentMaskButton.down ? Theme.buttonGhostPressed : editCurrentMaskButton.hovered ? Theme.buttonGhostHover : Theme.transparent
            }
            contentItem: RowLayout {
                spacing: 10
                ShadowIcon {
                    source: "qrc:/icons/mask.svg"
                    color: Theme.textSecondary
                    size: 19
                }
                Label {
                    Layout.fillWidth: true
                    text: editCurrentMaskButton.text
                    color: Theme.textPrimary
                    font.pixelSize: 11
                    font.weight: Font.Medium
                }
            }
            onClicked: {
                menu.close();
                menu.editExistingRequested();
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.preferredHeight: visible ? 1 : 0
            visible: editCurrentMaskButton.visible
            color: Theme.border
        }

        Label {
            Layout.fillWidth: true
            leftPadding: 8
            rightPadding: 8
            topPadding: 3
            bottomPadding: 1
            text: qsTr("APPLY TO")
            color: Theme.textMuted
            font.pixelSize: 9
            font.weight: Font.DemiBold
            font.letterSpacing: 0.7
        }

        Rectangle {
            id: destinationSelector
            objectName: "maskDestinationSelector"

            Layout.fillWidth: true
            Layout.preferredHeight: 38
            color: Theme.transparent

            RowLayout {
                anchors.fill: parent
                spacing: 0

                ShadowTabButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: menu.destination === menu.currentNodeDestination
                    text: qsTr("CURRENT NODE")
                    minimumTabWidth: 108
                    underlineInset: 18
                    enabled: menu.currentNodeAvailable
                    toolTipText: menu.currentMaskKind === 0 ? qsTr("Attach the mask to the selected Grade Node") : qsTr("The selected Grade Node already has a mask")
                    onClicked: menu.destination = menu.currentNodeDestination
                }

                ShadowTabButton {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: menu.destination === menu.newNodeDestination
                    text: qsTr("NEW NODE")
                    minimumTabWidth: 108
                    underlineInset: 18
                    enabled: menu.newNodeAvailable
                    toolTipText: qsTr("Create and select a new masked Grade Node")
                    onClicked: menu.destination = menu.newNodeDestination
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.topMargin: 3
            Layout.preferredHeight: 1
            color: Theme.border
        }

        Label {
            Layout.fillWidth: true
            leftPadding: 8
            rightPadding: 8
            topPadding: 3
            bottomPadding: 1
            text: qsTr("MASK TYPE")
            color: Theme.textMuted
            font.pixelSize: 9
            font.weight: Font.DemiBold
            font.letterSpacing: 0.7
        }

        ShadowButton {
            objectName: "aiSubjectMaskAction"
            Layout.fillWidth: true
            text: qsTr("AI subject")
            variant: ShadowButton.Secondary
            enabled: menu.currentNodeAvailable
            toolTipText: qsTr("Prompt SAM 2.1 on the selected Grade Node")
            onClicked: menu.startAiMask()
        }

        MaskAction {
            objectName: "brushMaskAction"
            text: qsTr("Brush")
            iconSource: "qrc:/icons/brush.svg"
            maskKind: 3
            enabled: menu.destination === menu.currentNodeDestination ? menu.currentNodeAvailable : menu.newNodeAvailable
        }

        MaskAction {
            text: qsTr("Linear gradient")
            iconSource: "qrc:/icons/mask-linear.svg"
            maskKind: 1
            enabled: menu.destination === menu.currentNodeDestination ? menu.currentNodeAvailable : menu.newNodeAvailable
        }

        MaskAction {
            text: qsTr("Radial gradient")
            iconSource: "qrc:/icons/mask-radial.svg"
            maskKind: 2
            enabled: menu.destination === menu.currentNodeDestination ? menu.currentNodeAvailable : menu.newNodeAvailable
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.topMargin: 4
            Layout.preferredHeight: 1
            color: Theme.border
        }

        Label {
            Layout.fillWidth: true
            leftPadding: 8
            rightPadding: 8
            topPadding: 3
            bottomPadding: 1
            text: qsTr("CONDITION")
            color: Theme.textMuted
            font.pixelSize: 9
            font.weight: Font.DemiBold
            font.letterSpacing: 0.7
        }

        MaskAction {
            text: qsTr("Luminance range")
            iconSource: "qrc:/icons/mask-luminance-range.svg"
            maskKind: 4
            enabled: menu.destination === menu.currentNodeDestination ? menu.currentNodeAvailable : menu.newNodeAvailable
        }

        MaskAction {
            text: qsTr("Color range")
            iconSource: "qrc:/icons/mask-color-range.svg"
            maskKind: 5
            enabled: menu.destination === menu.currentNodeDestination ? menu.currentNodeAvailable : menu.newNodeAvailable
        }
    }
}
