import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Lucidlight 1.0

// The script workbench: editor with JavaScript highlighting, run and stop,
// and an output pane fed by print() and errors.
ColumnLayout {
    id: root
    spacing: 6

    JsHighlighter {
        id: highlighter
        document: editor.textDocument
        keywordColor: theme.magenta
        dlColor: theme.accent
        stringColor: theme.green
        commentColor: Qt.alpha(theme.muted, 0.85)
        numberColor: theme.orange
    }

    function runScript() {
        if (editor.text.trimmed() === "")
            return;
        backend.scripts.run(editor.text);
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Button {
            id: runButton
            enabled: !backend.scripts.running
            Layout.preferredHeight: 30
            contentItem: RowLayout {
                spacing: 6
                Text { text: "▶"; font.pixelSize: 11; color: enabled ? theme.green : theme.muted }
                Label { text: qsTr("Run"); font.pixelSize: 12; color: runButton.enabled ? theme.bright : theme.muted }
            }
            background: Rectangle {
                radius: Math.min(6, theme.radius + 2)
                color: theme.surfaceAlt
                border.color: Qt.alpha(theme.green, 0.4)
            }
            onClicked: root.runScript()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Run the script (Ctrl+R)")
        }
        Button {
            id: stopButton
            enabled: backend.scripts.running
            Layout.preferredHeight: 30
            contentItem: RowLayout {
                spacing: 6
                Text { text: "■"; font.pixelSize: 11; color: theme.red }
                Label { text: qsTr("Stop"); font.pixelSize: 12; color: stopButton.enabled ? theme.bright : theme.muted }
            }
            background: Rectangle {
                radius: Math.min(6, theme.radius + 2)
                color: theme.surfaceAlt
                border.color: Qt.alpha(theme.muted, 0.3)
            }
            onClicked: backend.scripts.stop()
        }
        Label {
            Layout.fillWidth: true
            text: backend.scripts.running ? qsTr("Running…") : qsTr("Ready")
            color: backend.scripts.running ? theme.yellow : theme.muted
            font.pixelSize: 11
        }
        Label {
            text: qsTr("DL.Send · DL.ReceiveStr · DL.Wait · DL.Comment …")
            color: Qt.alpha(theme.muted, 0.6)
            font.family: win.monoFont
            font.pixelSize: 10
        }
    }

    // Editor with a line-number gutter. The minimum keeps the workbench
    // usable even when the window is small; it grows with the window.
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 170
        implicitHeight: 170

        Rectangle {
            anchors.fill: parent
            color: theme.deep
            radius: Math.min(8, theme.radius + 4)
            border.color: editor.activeFocus ? Qt.alpha(theme.accent, 0.4) : Qt.alpha(theme.muted, 0.25)
        }

        Flickable {
            id: flick
            anchors.fill: parent
            anchors.margins: 6
            clip: true
            contentWidth: editor.contentWidth
            contentHeight: editor.contentHeight
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}

            // Gutter with line numbers.
            Column {
                x: 6
                y: 8
                width: 30
                spacing: 0
                Repeater {
                    model: editor.lineCount
                    Label {
                        required property int index
                        width: 30
                        horizontalAlignment: Text.AlignRight
                        text: index + 1
                        color: Qt.alpha(theme.muted, 0.5)
                        font.family: win.monoFont
                        font.pixelSize: 11
                    }
                }
            }

            TextArea {
                id: editor
                x: 44
                width: flick.contentWidth - 44
                height: flick.contentHeight
                text: backend.scriptSource
                onTextChanged: if (text !== backend.scriptSource) backend.scriptSource = text
                font.family: win.monoFont
                font.pixelSize: 12
                color: theme.fg
                wrapMode: TextArea.NoWrap
                textFormat: TextEdit.PlainText
                selectByMouse: true
                persistentSelection: true
                cursorDelegate: Rectangle {
                    visible: editor.activeFocus
                    width: 2
                    color: theme.accent
                }
                background: null
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Tab) {
                        editor.insert(editor.cursorPosition, "  ");
                        event.accepted = true;
                    }
                }
            }
        }
    }

    // Output pane.
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 130
        color: theme.surfaceAlt
        radius: Math.min(6, theme.radius + 2)
        border.color: Qt.alpha(theme.muted, 0.2)

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 4
                spacing: 6
                Label {
                    text: qsTr("Output")
                    color: theme.muted
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                ToolButton {
                    text: "✕"
                    font.pixelSize: 10
                    onClicked: outputModel.clear()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Clear output")
                }
            }

            ListView {
                id: outputList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: ListModel { id: outputModel }
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: Label {
                    required property int index
                    required property string line
                    required property bool isError
                    width: outputList.width - 16
                    x: 8
                    text: line
                    color: isError ? theme.red : theme.fg
                    font.family: win.monoFont
                    font.pixelSize: 11
                    wrapMode: Text.NoWrap
                    elide: Text.ElideRight
                }
                onCountChanged: positionViewAtEnd()
            }
        }
    }

    Connections {
        target: backend.scripts
        function onOutputLine(line, error) {
            outputModel.append({ line: line, isError: error });
            if (outputModel.count > 500)
                outputModel.remove(0, 100);
        }
        function onMessageRequested(text) {
            outputModel.append({ line: "· " + text, isError: false });
        }
    }
}
