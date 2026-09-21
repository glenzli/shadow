pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property var editor
    readonly property var controller: editor.autoStart
    implicitWidth: entry.implicitWidth
    implicitHeight: entry.implicitHeight

    ShadowButton {
        id: entry
        objectName: "autoStartEntry"
        compact: true
        variant: ShadowButton.Tinted
        text: qsTr("Auto")
        selected: root.controller.ready
        enabled: root.editor.active && !root.editor.stateBusy
        toolTipText: root.controller.ready ? qsTr("Review the prepared suggestion")
            : qsTr("Automatically adjust white balance, tone and skin. Processed locally.")
        onClicked: {
            dialog.open()
            if (!root.controller.active && !root.controller.busy)
                root.controller.analyze()
        }
        // Fixed entry geometry while an optional background suggestion runs.
        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 3
            width: 5
            height: 5
            radius: 3
            color: Theme.accent
            visible: root.controller.busy
        }
    }

    Popup {
        id: dialog
        objectName: "autoStartDialog"
        parent: Overlay.overlay
        width: Math.min(560, Math.max(0, parent.width - 48))
        height: Math.min(640, Math.max(0, parent.height - 48))
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        padding: 0
        modal: false
        dim: false
        focus: true
        closePolicy: root.controller.applying ? Popup.NoAutoClose : Popup.CloseOnEscape
        background: Rectangle {
            color: Theme.panelRaised
            radius: 12
            border.color: Theme.borderStrong
        }
        contentItem: ColumnLayout {
            spacing: 0
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                color: Theme.transparent
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.panelPadding
                    anchors.rightMargin: 10
                    spacing: 8
                    Label {
                        objectName: "autoStartTitle"
                        Layout.fillWidth: true
                        text: qsTr("Auto")
                        font.pixelSize: Theme.fontTitle
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                    }
                    Rectangle {
                        width: 5
                        height: 5
                        radius: 3
                        color: Theme.accent
                        opacity: root.controller.busy ? 1 : 0
                    }
                    Label {
                        text: qsTr("Local processing")
                        font.pixelSize: Theme.fontMeta
                        color: Theme.textMuted
                    }
                    ShadowIconButton {
                        source: "qrc:/icons/close.svg"
                        accessibleName: qsTr("Close")
                        toolTipText: qsTr("Hide suggestions and continue editing")
                        enabled: !root.controller.applying
                        onClicked: dialog.close()
                    }
                }
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Theme.border
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.panelPadding
                spacing: 12
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(100, Math.min(230, dialog.height * 0.37))
                    color: Theme.photoCanvas
                    radius: Theme.controlRadius
                    Image {
                        objectName: "autoStartPreview"
                        anchors.fill: parent
                        anchors.margins: 1
                        source: compare.down ? root.controller.originalSource :
                            (root.controller.previewSource.length ? root.controller.previewSource : root.controller.originalSource)
                        fillMode: Image.PreserveAspectFit
                        cache: false
                    }
                    ShadowButton {
                        id: compare
                        objectName: "autoStartCompare"
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.margins: 8
                        compact: true
                        variant: ShadowButton.Secondary
                        text: down ? qsTr("Before") : qsTr("Hold to compare")
                        minimumButtonWidth: 128
                        enabled: root.controller.ready
                    }
                }
                ScrollView {
                    id: controlsScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ColumnLayout {
                        width: controlsScroll.availableWidth
                        spacing: 10
                        Label {
                            Layout.fillWidth: true
                            text: root.controller.status
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 20
                            ShadowCheckBox {
                                objectName: "autoStartWhiteBalance"
                                text: qsTr("White balance")
                                checked: root.controller.whiteBalanceEnabled
                                enabled: root.controller.whiteBalanceAvailable && !root.controller.applying
                                onToggled: root.controller.whiteBalanceEnabled = checked
                            }
                            ShadowCheckBox {
                                objectName: "autoStartTone"
                                text: qsTr("Tone")
                                checked: root.controller.toneEnabled
                                enabled: root.controller.toneAvailable && !root.controller.applying
                                onToggled: root.controller.toneEnabled = checked
                            }
                            ShadowCheckBox {
                                objectName: "autoStartSkin"
                                text: qsTr("Skin")
                                checked: root.controller.skinEnabled
                                enabled: root.controller.skinAvailable && !root.controller.applying
                                onToggled: root.controller.skinEnabled = checked
                            }
                        }
                        ShadowSlider {
                            objectName: "autoStartStrength"
                            Layout.fillWidth: true
                            label: qsTr("Strength")
                            from: 0
                            to: 1
                            stepSize: 0.01
                            displayMultiplier: 100
                            decimals: 0
                            suffix: "%"
                            value: root.controller.strength
                            enabled: root.controller.ready && !root.controller.applying
                            onEdited: value => root.controller.strength = value
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: root.controller.ready
                            text: root.controller.summary
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontMeta
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                        }
                        ShadowSwitch {
                            objectName: "autoStartAutomatic"
                            compact: true
                            Layout.fillWidth: true
                            text: qsTr("Prepare suggestions for unedited photos")
                            checked: root.controller.automaticEnabled
                            onToggled: root.controller.automaticEnabled = checked

                        }
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.border
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.panelPadding
                spacing: 8
                ShadowButton {
                    compact: true
                    text: qsTr("Cancel")
                    variant: ShadowButton.Ghost
                    enabled: !root.controller.applying
                    onClicked: { root.controller.cancel(); dialog.close() }
                }
                ShadowIconButton {
                    objectName: "autoStartRetry"
                    source: "qrc:/icons/reset-all.svg"
                    accessibleName: qsTr("Analyze again")
                    toolTipText: accessibleName
                    enabled: !root.controller.busy
                    onClicked: { root.controller.cancel(); root.controller.analyze() }
                }
                Item { Layout.fillWidth: true }
                ShadowButton {
                    objectName: "autoStartApply"
                    compact: true
                    text: qsTr("Apply")
                    toolTipText: qsTr("Apply the current preview; unfinished analysis stops.")
                    variant: ShadowButton.Primary
                    enabled: root.controller.canApply
                    onClicked: root.controller.apply()
                }
            }
        }
        Connections {
            target: root.controller
            function onApplied() { dialog.close() }
        }
    }
    Connections {
        target: root.editor
        function onSourceIdentityChanged() { dialog.close() }
    }

}
