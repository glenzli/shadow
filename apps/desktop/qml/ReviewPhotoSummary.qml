import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Owns the selected visual's compact identity and dimensions summary.
ColumnLayout {
    id: summary

    required property var review

    function localizedVisualRole(role) {
        switch (String(role).trim().toLowerCase()) {
        case "recipe":
            return qsTranslate("ReviewWorkspace", "RECIPE")
        case "proxy":
            return qsTranslate("ReviewWorkspace", "PROXY")
        default:
            return String(role).toUpperCase()
        }
    }

    spacing: 10

    Label {
        text: qsTranslate("ReviewWorkspace", "PHOTO")
        color: summary.review.textMuted
        font.pixelSize: Theme.fontMeta
        font.weight: Font.DemiBold
        font.letterSpacing: 1.6
    }

    Label {
        Layout.fillWidth: true
        text: summary.review.selectedTitle.length > 0
            ? summary.review.selectedTitle
            : qsTranslate("ReviewWorkspace", "Nothing selected")
        color: summary.review.textPrimary
        font.pixelSize: Theme.fontTitle
        font.weight: Font.Medium
        elide: Text.ElideRight
    }

    Label {
        Layout.fillWidth: true
        text: summary.review.selectedPath
        color: summary.review.textMuted
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.NoWrap
        maximumLineCount: 1
        elide: Text.ElideMiddle
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: summary.review.border
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Label {
            text: summary.review.selectedRole.length > 0
                ? summary.localizedVisualRole(summary.review.selectedRole)
                : qsTranslate("ReviewWorkspace", "PENDING")
            color: summary.review.textPrimary
            font.pixelSize: Theme.fontMeta
            font.weight: Font.Medium
        }

        Rectangle {
            Layout.preferredWidth: 3
            Layout.preferredHeight: 3
            radius: 1.5
            color: summary.review.textMuted
        }

        Label {
            Layout.fillWidth: true
            text: summary.review.selectedWidth > 0
                ? qsTranslate("ReviewWorkspace", "%L1 × %L2")
                    .arg(summary.review.selectedWidth)
                    .arg(summary.review.selectedHeight)
                : "—"
            color: summary.review.textMuted
            font.pixelSize: Theme.fontMeta
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: summary.review.border
    }
}
