pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var editor
    readonly property var controller: editor.subjectEmphasis
    Layout.fillWidth: true
    Layout.leftMargin: 12
    Layout.rightMargin: 12
    Layout.topMargin: 4
    Layout.bottomMargin: 4
    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: qsTr("Assisted editing")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSection
            font.weight: Font.DemiBold
        }
        PrecisionAutoStart { editor: root.editor }
        ShadowIconButton {
            objectName: "analyzeSubjectEmphasis"
            source: "qrc:/icons/scopes.svg"
            visible: !root.controller.active
            accessibleName: qsTr("Subject emphasis")
            toolTipText: accessibleName + "\n" + qsTr("Local AI · Preview") + "\n"
                + qsTr("Use local QwenVL to describe this photo, then choose the subject. No cloud upload.")
            enabled: root.editor.active && !root.editor.stateBusy && !root.controller.busy
            onClicked: root.controller.analyze()
        }
    }
    Label {
        Layout.fillWidth: true
        visible: root.controller.status.length > 0
        text: root.controller.status
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontMeta
        textFormat: Text.PlainText
    }
    BusyIndicator {
        Layout.alignment: Qt.AlignHCenter
        implicitWidth: 26
        implicitHeight: 26
        visible: root.controller.busy
        running: visible
    }
    Item {
        Layout.fillWidth: true
        Layout.preferredHeight: visible ? 145 : 0
        visible: root.controller.active && root.controller.analyzed
        Image {
            id: photo
            anchors.fill: parent
            source: root.controller.imageSource
            fillMode: Image.PreserveAspectFit
            cache: false
            Image {
                anchors.centerIn: parent
                width: photo.paintedWidth
                height: photo.paintedHeight
                source: root.controller.maskSource
                fillMode: Image.Stretch
                cache: false
            }
        }
    }
    Label {
        Layout.fillWidth: true
        visible: root.controller.active && root.controller.analyzed
        text: root.controller.description
        textFormat: Text.PlainText
        maximumLineCount: 3
        elide: Text.ElideRight
        wrapMode: Text.Wrap
        color: Theme.textMuted
        font.pixelSize: Theme.fontCaption
    }
    Flow {
        Layout.fillWidth: true
        spacing: 8
        visible: root.controller.active && root.controller.analyzed
        Repeater {
            model: root.controller.queries
            ShadowButton {
                required property string modelData
                compact: true
                variant: ShadowButton.Ghost
                text: modelData
                enabled: !root.controller.busy
                onClicked: {
                    query.text = modelData
                    root.controller.selectSubject(modelData)
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: root.controller.active && root.controller.analyzed
        ShadowTextField {
            id: query
            objectName: "subjectEmphasisQuery"
            Layout.fillWidth: true
            placeholderText: qsTr("Subject, e.g. bird")
            maximumLength: 128
            enabled: !root.controller.busy
            onAccepted: root.controller.selectSubject(text)
        }
        ShadowIconButton {
            source: "qrc:/icons/check.svg"
            accessibleName: qsTr("Select")
            toolTipText: accessibleName
            enabled: query.text.trim().length > 0 && !root.controller.busy
            onClicked: root.controller.selectSubject(query.text)
        }
    }
    ShadowSlider {
        Layout.fillWidth: true
        visible: root.controller.canApply || root.controller.applied
            || (root.controller.active && root.controller.maskSource.length > 0)
        label: qsTr("Strength")
        from: 0
        to: 1
        stepSize: 0.01
        displayMultiplier: 100
        decimals: 0
        suffix: "%"
        value: root.controller.strength
        enabled: !root.controller.busy && (root.controller.active || !root.editor.stateBusy)
        onGestureStarted: if (root.controller.applied) root.editor.beginParameterEdit("node/strength")
        onEdited: value => root.controller.strength = value
        onGestureFinished: if (root.controller.applied) root.editor.endParameterEdit("node/strength")
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: root.controller.active
        ShadowButton {
            compact: true
            variant: ShadowButton.Ghost
            text: qsTr("Cancel")
            enabled: !root.controller.applying
            onClicked: root.controller.cancel()
        }
        Item { Layout.fillWidth: true }
        ShadowButton {
            compact: true
            objectName: "applySubjectEmphasis"
            text: qsTr("Apply emphasis")
            variant: ShadowButton.Primary
            enabled: root.controller.canApply
            onClicked: root.controller.apply()
        }
    }
}
