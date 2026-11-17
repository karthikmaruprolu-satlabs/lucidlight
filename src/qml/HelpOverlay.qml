import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The keyboard map, shown for the ? key.
Popup {
    id: root

    width: 560
    height: implicitContentHeight + 44
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
            columns: 1
            columnSpacing: 26
            rowSpacing: 10
            Layout.fillWidth: true

            Repeater {
                model: [
                    ["Space / F5 / F6", qsTr("Start / stop communication")],
                    ["Enter", qsTr("Send the typed data")],
                    ["F2 / F3", qsTr("Start / stop file logging")],
                    ["Ctrl+F1 – F12", qsTr("Send the sequence bound to the key, anywhere")],
                    ["F12", qsTr("Show / hide the script area")],
                    ["Shift+F12", qsTr("Show / hide the sequence panel")],
                    ["Ctrl+F5 / Ctrl+F6", qsTr("Keyboard console on / off")],
                    ["Shift+F5 / Ctrl+R", qsTr("Run the script")],
                    ["Shift+F6", qsTr("Stop the script (and dispatched scripts)")],
                    ["Ctrl+T", qsTr("Save the script")],
                    ["Ctrl+K / Ctrl+W", qsTr("Clear the window")],
                    ["Ctrl+F", qsTr("Search in the data")],
                    ["Ctrl+B", qsTr("Toggle the sequence panel")],
                    ["Ctrl+O / Ctrl+S", qsTr("Open / save the project")],
                    ["Ctrl+Shift+S", qsTr("Save the communication log")],
                    ["Ctrl+P", qsTr("Print the communication window")],
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
                        Layout.preferredWidth: 170
                    }
                    Label {
                        text: modelData[1]
                        color: theme.fg
                        font.pixelSize: 12
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
