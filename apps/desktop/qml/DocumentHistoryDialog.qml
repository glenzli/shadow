pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ShadowDialog {
    id: dialog
    required property var history
    title: qsTr("Document history")
    width: 480
    height: 560
    modal: true
    onOpened: history.refresh()
    contentItem: ColumnLayout {
        spacing: 12
        Label {
            Layout.fillWidth: true
            text: qsTr("Restore an earlier edit to continue from it. Later edits remain in the document.")
            wrapMode: Text.Wrap
            color: Theme.textSecondary
        }
        BusyIndicator { running: dialog.history.busy; visible: running; Layout.alignment: Qt.AlignHCenter }
        Label { text: dialog.history.error; visible: text.length > 0; color: Theme.errorText; wrapMode: Text.Wrap }
        Label { text: qsTr("No completed edits yet."); visible: !dialog.history.busy && historyList.count === 0; color: Theme.textSecondary }
        ListView {
            id: historyList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.history.items
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property var modelData
                width: ListView.view.width
                text: modelData.label + (modelData.current ? qsTr(" · Current") : "")
                enabled: !dialog.history.busy
                onClicked: dialog.history.restore(modelData.id)
            }
        }
    }
    footer: DialogButtonBox {
        ShadowButton {
            text: qsTr("Load earlier edits")
            visible: dialog.history.hasMore
            enabled: !dialog.history.busy
            onClicked: dialog.history.loadMore()
        }
        ShadowButton { text: qsTr("Close"); onClicked: dialog.close() }
    }
}
