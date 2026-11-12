import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The send bar: notation input with a live preview and the send action.
ColumnLayout {
    id: root
    spacing: 6

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        ComboBox {
            id: formatBox
            Layout.preferredWidth: 100
            font.pixelSize: 12
            model: [qsTr("Text"), qsTr("Hex"), qsTr("Dec"), qsTr("Binary")]
        }

        TextField {
            id: sendField
            Layout.fillWidth: true
            placeholderText: qsTr("Data to send — [0D] inserts a hex byte, [[ a literal [")
            font.family: win.monoFont
            font.pixelSize: 13
            color: theme.fg
            selectByMouse: true

            Keys.onReturnPressed: send()
            Keys.onEnterPressed: send()

            function send() {
                if (backend.sendText(text, formatBox.currentIndex)) {
                    clear();
                    preview.text = "";
                }
            }
        }

        Button {
            Layout.preferredHeight: 36
            Layout.preferredWidth: 92
            highlighted: true
            enabled: sendField.text !== ""
            contentItem: Label {
                text: qsTr("Send ⏎")
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 12
                font.weight: Font.DemiBold
                color: backend.running ? "black" : theme.muted
            }
            background: Rectangle {
                radius: Math.min(8, theme.radius + 4)
                color: backend.running ? theme.accent : theme.surface
                border.color: backend.running ? theme.accent : Qt.alpha(theme.muted, 0.35)
            }
            onClicked: sendField.send()
        }
    }

    Label {
        id: preview
        Layout.fillWidth: true
        text: {
            const input = sendField.text;
            if (input === "")
                return "";
            const error = backend.parseError(input, formatBox.currentIndex);
            if (error !== "")
                return qsTr("Invalid: %1").arg(error);
            const hex = backend.previewBytes(input, formatBox.currentIndex);
            const count = hex === "" ? 0 : hex.split(" ").length;
            return hex + "   ·   " + qsTr("%n byte(s)", "", count) + " out";
        }
        visible: text !== ""
        color: backend.parseError(sendField.text, formatBox.currentIndex) !== "" ? theme.red : theme.muted
        font.family: win.monoFont
        font.pixelSize: 11
        wrapMode: Text.WrapAnywhere
    }
}
