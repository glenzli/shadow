pragma ComponentBehavior: Bound
pragma Translator: "PrecisionWorkspace"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Named immutable checkpoints are intentionally isolated from the live
// autosaved adjustment surface so the future History workspace can evolve
// without growing the Precision inspector.
            Item {
    id: versionsPane
    required property var inspector
                // Transitional implementation retained for the future
                // History/checkpoint migration. It has no entry point
                // in Precision.
                visible: false
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 10

                    Label {
                        text: qsTr("CREATE VERSION CHECKPOINT")
                        color: inspector.textMuted
                        font.pixelSize: Theme.fontMeta
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.2
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: draftSummary.implicitHeight + 20
                        radius: Theme.controlRadius
                        color: inspector.editor.autosaveFailed ? Theme.dangerSurface
                            : inspector.editor.dirty ? Theme.accentSurfaceQuiet
                            : Theme.panelRaised
                        border.color: inspector.editor.autosaveFailed ? Theme.errorBorder
                            : inspector.editor.dirty ? Theme.accentBorder : inspector.panelBorder

                        ColumnLayout {
                            id: draftSummary
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 3

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6

                                Label {
                                    Layout.fillWidth: true
                                    text: !inspector.editor.dirty ? qsTr("CURRENT AUTOSAVE")
                                        : inspector.editor.autosaveFailed ? qsTr("AUTOSAVE FAILED")
                                        : inspector.editor.autosavePending
                                            ? qsTr("AUTOSAVE PENDING") : qsTr("VERSION DRAFT")
                                    color: !inspector.editor.dirty ? inspector.textSecondary
                                        : inspector.editor.autosaveFailed ? Theme.errorText
                                        : inspector.accent
                                    font.pixelSize: Theme.fontCaption
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 0.8
                                }

                                ShadowIconButton {
                                    visible: inspector.editor.autosaveFailed
                                    source: "qrc:/icons/redo.svg"
                                    iconSize: 15
                                    buttonSize: Theme.compactControlHeight
                                    toolTipText: qsTr("Retry autosave")
                                    accessibleName: toolTipText
                                    onClicked: inspector.editor.retryAutosave()
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: inspector.editor.autosaveFailed
                                    ? inspector.editor.autosaveErrorText
                                    : qsTr("Adjustments save automatically to this photo’s current working state. Creating a version adds a named, immutable Library checkpoint; only those checkpoints appear below.")
                                color: inspector.editor.autosaveFailed
                                    ? Theme.errorText : inspector.textMuted
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    ShadowTextField {
                        id: versionLabel
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.controlHeight
                        enabled: inspector.editor.active && !inspector.editor.stateBusy
                        placeholderText: qsTr("Version name")
                        color: inspector.textPrimary
                        placeholderTextColor: Theme.textPlaceholder
                        selectByMouse: true
                        background: Rectangle {
                            radius: Theme.controlRadius
                            color: Theme.panelRaised
                            border.color: versionLabel.activeFocus ? inspector.accent : inspector.panelBorder
                        }
                        onAccepted: {
                            const cleanLabel = text.trim()
                            if (cleanLabel.length > 0 && saveButton.enabled) {
                                inspector.editor.saveVersion(cleanLabel)
                                clear()
                            }
                        }
                    }

                    ShadowButton {
                        id: saveButton
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.controlHeight
                        text: inspector.editor.stateBusy
                            ? qsTr("CREATING…") : qsTr("CREATE VERSION")
                        variant: ShadowButton.Primary
                        enabled: inspector.editor.active && !inspector.editor.stateBusy
                            && versionLabel.text.trim().length > 0
                        onClicked: {
                            inspector.editor.saveVersion(versionLabel.text.trim())
                            versionLabel.clear()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: inspector.panelBorder
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("NAMED VERSIONS")
                            color: inspector.textMuted
                            font.pixelSize: Theme.fontMeta
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.2
                        }
                        Label {
                            text: qsTr("%L1").arg(versionList.count)
                            color: inspector.textMuted
                            font.pixelSize: Theme.fontCaption
                        }
                    }

                    ListView {
                        id: versionList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: inspector.editor.versions
                        spacing: 7
                        clip: true

                        delegate: Rectangle {
                            id: versionRow
                            required property string commitId
                            required property string label
                            required property string createdAtText
                            required property bool selected
                            required property int parentCount
                            required property string changeSummary
                            required property string parentSummary

                            width: versionList.width
                            height: 78
                            radius: Theme.controlRadius
                            color: selected
                                ? Theme.currentRevisionSurface : Theme.panelRaised
                            border.color: selected
                                ? Theme.currentRevisionBorder : inspector.panelBorder

                            Column {
                                anchors.left: parent.left
                                anchors.right: currentBadge.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 11
                                anchors.rightMargin: 8
                                spacing: 4
                                Label {
                                    width: parent.width
                                    text: versionRow.label
                                    color: inspector.textPrimary
                                    font.pixelSize: Theme.fontSection
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    width: parent.width
                                    text: versionRow.changeSummary
                                    color: inspector.textSecondary
                                    font.pixelSize: Theme.fontMeta
                                    elide: Text.ElideRight
                                }
                                Label {
                                    width: parent.width
                                    text: qsTr("%1  ·  %2")
                                        .arg(versionRow.createdAtText)
                                        .arg(versionRow.parentSummary)
                                    color: inspector.textMuted
                                    font.pixelSize: Theme.fontCaption
                                    elide: Text.ElideRight
                                }
                            }

                            Label {
                                id: currentBadge
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: versionRow.selected
                                    ? (inspector.editor.versionDraft
                                        ? qsTr("LOADED") : qsTr("CURRENT"))
                                    : qsTr("LOAD")
                                color: versionRow.selected ? inspector.accent : inspector.textMuted
                                font.pixelSize: Theme.fontMicro
                                font.weight: Font.Bold
                                font.letterSpacing: 0.7
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: !versionRow.selected && !inspector.editor.stateBusy
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: inspector.editor.loadVersionDraft(versionRow.commitId)
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 20
                            visible: versionList.count === 0
                            text: qsTr("Named checkpoints appear here. The current working adjustments are saved automatically; loading a checkpoint never deletes newer work.")
                            color: inspector.textMuted
                            font.pixelSize: Theme.fontMeta
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            lineHeight: 1.35
                        }
                    }
                }
            }
