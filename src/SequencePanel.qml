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
        list.model = backend.sequences;
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
            model: backend.sequences
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                required property int index
                required property string name
                required property bool isSend
                required property string preview
                required property int hotkey
                required property bool seqEnabled
                width: list.width
                height: 44

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
                            text: hotkey > 0 ? "F" + hotkey : (isSend ? "→" : "↺")
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
                            text: "<span style=\"color:" + Qt.rgba(theme.muted.r, theme.muted.g, theme.muted.b, 0.7) + "\">[" + index + "]</span>  " + name
                            color: seqEnabled ? theme.bright : theme.muted
                            font.pixelSize: 12
                            textFormat: Text.RichText
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: preview
                            color: Qt.alpha(theme.muted, 0.8)
                            font.pixelSize: 10
                            font.family: win.monoFont
                            elide: Text.ElideMiddle
                        }
                    }
                    ToolButton {
                        text: "✎"
                        font.pixelSize: 11
                        flat: true
                        onClicked: root.requestEdit(index)
                    }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.RightButton) {
                            root.requestEdit(index);
                        } else if (isSend) {
                            backend.sendSequence(index);
                        } else {
                            root.requestEdit(index);
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
            text: qsTr("F1–F12 send their sequence anywhere")
            color: Qt.alpha(theme.muted, 0.7)
            font.pixelSize: 10
            wrapMode: Text.Wrap
        }
    }
}
