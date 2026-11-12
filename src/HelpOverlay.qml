import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The keyboard map, shown for the ? key.
Popup {
    id: root

    width: 480
    height: 460
    anchors.centerIn: parent
    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: theme.surface
        radius: Math.min(12, theme.radius + 4)
        border.color: Qt.alpha(theme.muted, 0.35)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 14

        Label {
            text: qsTr("Keyboard")
            font.pixelSize: 17
            font.weight: Font.DemiBold
            color: theme.accent
        }

        GridLayout {
            columns: 2
            columnSpacing: 26
            rowSpacing: 8
            Layout.fillWidth: true

            Repeater {
                model: [
                    ["Space", qsTr("Start / stop communication")],
                    ["Enter", qsTr("Send the typed data")],
                    ["F1 – F12", qsTr("Send the sequence bound to the key")],
                    ["Ctrl+R", qsTr("Run the script")],
                    ["Ctrl+K", qsTr("Clear the window")],
                    ["Ctrl+F", qsTr("Search in the data")],
                    ["Ctrl+B", qsTr("Toggle the sequence panel")],
                    ["Ctrl+O / Ctrl+S", qsTr("Open / save the project")],
                    ["Ctrl+Shift+S", qsTr("Save the communication log")],
                    ["?", qsTr("This help")],
                    ["Ctrl+Q", qsTr("Quit")]
                ]
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label {
                        text: modelData[0]
                        color: theme.accent
                        font.family: win.monoFont
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        Layout.preferredWidth: 150
                    }
                    Label {
                        text: modelData[1]
                        color: theme.fg
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Data notation: [0D] inserts a hex byte, [[ a literal bracket, and ?? matches one unknown byte in receive patterns.")
            color: theme.muted
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
}
