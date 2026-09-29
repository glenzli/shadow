pragma ComponentBehavior: Bound
pragma Translator: PrecisionWorkspace

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Bounded semantic-intent authoring. Display labels are localized while the
// preset payloads remain stable model queries that can be rerun on any photo.
ColumnLayout {
    id: panel

    property string query: ""
    property bool startEnabled: true
    signal startRequested(string query)

    spacing: 7

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Choose what the mask should follow. Copying this mask reruns the same meaning on the target photo.")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontMeta
    }

    Flow {
        Layout.fillWidth: true
        spacing: 5

        Repeater {
            model: [
                { label: qsTr("Sky"), query: "sky" },
                { label: qsTr("Person"), query: "person" },
                { label: qsTr("Face"), query: "face" },
                { label: qsTr("Hair"), query: "hair" },
                { label: qsTr("Clothing"), query: "clothing" },
                { label: qsTr("Vehicle"), query: "vehicle" },
                { label: qsTr("Building"), query: "building" },
                { label: qsTr("Vegetation"), query: "vegetation" },
                { label: qsTr("Water"), query: "water" },
                { label: qsTr("Background"), query: "background" }
            ]

            delegate: ShadowButton {
                id: preset
                required property var modelData
                compact: true
                text: preset.modelData.label
                variant: panel.query === preset.modelData.query
                    ? ShadowButton.Primary : ShadowButton.Ghost
                enabled: panel.startEnabled
                onClicked: panel.query = preset.modelData.query
            }
        }
    }

    ShadowTextField {
        id: customQuery
        objectName: "semanticMaskCustomQuery"
        Layout.fillWidth: true
        placeholderText: qsTr("Custom subject, for example: red train")
        text: panel.query
        maximumLength: 256
        enabled: panel.startEnabled
        selectByMouse: true
        onTextEdited: panel.query = text.trim()
        onAccepted: {
            if (panel.startEnabled && panel.query.length > 0)
                panel.startRequested(panel.query)
        }
    }

    ShadowButton {
        objectName: "semanticMaskStartButton"
        Layout.fillWidth: true
        text: qsTr("Preview semantic mask")
        variant: ShadowButton.Primary
        enabled: panel.startEnabled && panel.query.length > 0
        onClicked: panel.startRequested(panel.query)
    }
}
