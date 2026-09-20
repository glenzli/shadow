import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Library-side navigation index. Collection, album, import, and comparison
// lifecycles remain independently navigable in their semantic owners.
Rectangle {
    id: sidebar

    required property var workspace
    required property var albumDialogs

    Layout.preferredWidth: 210
    Layout.fillHeight: true
    color: sidebar.workspace.panel

    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 1
        color: sidebar.workspace.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.panelPadding
        anchors.rightMargin: Theme.panelPadding + 1
        anchors.topMargin: Theme.panelPadding
        anchors.bottomMargin: Theme.panelPadding
        spacing: 8

        Label {
            text: qsTranslate("ReviewWorkspace", "LIBRARY")
            color: sidebar.workspace.textMuted
            font.pixelSize: Theme.fontMeta
            font.weight: Font.DemiBold
            font.letterSpacing: 1.6
        }

        Item { Layout.preferredHeight: 6 }

        ReviewSystemCollections {
            Layout.fillWidth: true
            workspace: sidebar.workspace
        }

        ReviewDailyCollection {
            Layout.fillWidth: true
            workspace: sidebar.workspace
        }

        ReviewTravelCollections {
            Layout.fillWidth: true
            workspace: sidebar.workspace
        }

        ReviewSmartCategoryList {
            Layout.fillWidth: true
            workspace: sidebar.workspace
        }

        ReviewAlbumList {
            Layout.fillWidth: true
            workspace: sidebar.workspace
            albumDialogs: sidebar.albumDialogs
        }

        ReviewImportProgressCard {
            Layout.fillWidth: true
            workspace: sidebar.workspace
        }

        ReviewComparisonEvidence {
            Layout.fillWidth: true
            workspace: sidebar.workspace
        }

        Item { Layout.fillHeight: true }
    }
}
