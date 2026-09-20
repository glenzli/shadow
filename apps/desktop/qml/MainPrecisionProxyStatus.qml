import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Read-only presentation of the Precision proxy-rendering state.
RowLayout {
    id: proxyStatus

    required property bool active

    spacing: 5

    Label {
        text: qsTranslate("Main", "PROXY")
        color: Theme.textMuted
        font.pixelSize: Theme.fontCaption
        font.letterSpacing: 0.7
    }

    Rectangle {
        Layout.preferredWidth: 6
        Layout.preferredHeight: 6
        radius: 3
        color: proxyStatus.active ? Theme.accent : Theme.textMuted
    }

    Label {
        text: proxyStatus.active
            ? qsTranslate("Main", "ON")
            : qsTranslate("Main", "OFF")
        color: proxyStatus.active ? Theme.accent : Theme.textMuted
        font.pixelSize: Theme.fontCaption
        font.weight: Font.DemiBold
    }
}
