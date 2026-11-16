import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The left panel: send and receive sequences with their hotkeys.
Rectangle {
    id: root

    color: theme.surfaceAlt
    radius: Math.min(8, theme.radius + 4)

    signal requestEdit(int row)
    signal requestAdd(bool isSend)

    function refresh() {
        list.model = 0;
        list.model = backend.sequencesGrouped;
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Sequences")
                color: theme.bright
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                text: "+"
                font.pixelSize: 15
                onClicked: root.requestAdd(true)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("New send sequence")
            }
            ToolButton {
                text: "+↺"
                font.pixelSize: 13
                onClicked: root.requestAdd(false)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("New receive sequence")
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: backend.sequencesGrouped
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            // Grouped view: send sequences first, then receive sequences.
            // The underlying combined order is the scripting index, so the
            // delegates map their display row back before acting on it.
            section.property: "group"
            section.delegate: Item {
                required property string section
                width: list.width
                height: 22
                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: section === "receive" ? qsTr("Receive sequences") : qsTr("Send sequences")
                    color: section === "receive" ? theme.green : theme.blue
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.5
                    font.capitalization: Font.AllUppercase
                }
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: theme.muted
                    opacity: 0.25
                }
            }

            delegate: Item {
                required property int index
                required property string name
                required property bool isSend
                required property string preview
                required property string comment
                required property int hotkey
                required property bool seqEnabled
                // The combined scripting index (DL.SendSequence / the label
                // prefix), not the grouped display row.
                readonly property int srcIndex: backend.sequencesGrouped.sourceRow(index)
                width: list.width
                height: 44
                // Docklight shows the sequence documentation next to the
                // sequence in the list.
                ToolTip.visible: rowMouse.containsMouse
                                 && (comment !== "" || preview !== "")
                ToolTip.delay: 600
                ToolTip.text: (comment !== "" ? comment + "\n" : "") + preview

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    anchors.rightMargin: 2
                    radius: Math.min(6, theme.radius + 2)
                    color: rowMouse.containsMouse ? Qt.alpha(theme.muted, 0.12) : "transparent"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 8
                    spacing: 8

                    Rectangle {
                        width: 40
                        height: 18
                        radius: 4
                        color: isSend ? Qt.alpha(theme.blue, 0.15) : Qt.alpha(theme.green, 0.15)
                        Label {
                            anchors.centerIn: parent
                            text: hotkey > 0 ? "\u2303F" + hotkey : (isSend ? "→" : "↺")
                            color: isSend ? theme.blue : theme.green
                            font.pixelSize: 9
                            font.weight: Font.Bold
                            font.family: win.monoFont
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            // Docklight shows the 0-based sequence index.
                            text: "<span style=\"color:" + Qt.rgba(theme.muted.r, theme.muted.g, theme.muted.b, 0.7) + "\">[" + srcIndex + "]</span>  " + name
                            color: seqEnabled ? theme.bright : theme.muted
                            font.pixelSize: 12
                            textFormat: Text.RichText
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            // The documentation (comment) replaces the data
                            // preview when the user wrote one, like the
                            // Documentation column in Docklight.
                            text: comment !== "" ? comment : preview
                            color: comment !== ""
                                       ? Qt.rgba(theme.muted.r, theme.muted.g, theme.muted.b, 0.9)
                                       : Qt.alpha(theme.muted, 0.8)
                            font.pixelSize: 10
                            font.family: win.monoFont
                            font.italic: comment !== ""
                            elide: Text.ElideMiddle
                        }
                    }
                    ToolButton {
                        text: "✎"
                        font.pixelSize: 11
                        flat: true
                        onClicked: root.requestEdit(srcIndex)
                    }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.RightButton) {
                            root.requestEdit(srcIndex);
                        } else if (isSend) {
                            backend.sendSequence(srcIndex);
                        } else {
                            root.requestEdit(srcIndex);
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: backend.sequences.count === 0
                text: qsTr("No sequences yet")
                color: theme.muted
                font.pixelSize: 12
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Ctrl+F1–F12 send their sequence anywhere")
            color: Qt.alpha(theme.muted, 0.7)
            font.pixelSize: 10
            wrapMode: Text.Wrap
        }
    }
}
