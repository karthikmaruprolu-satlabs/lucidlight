import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Editor for one sequence: name, direction, notation, hotkey, repeat, and
// for receive sequences the send sequence to trigger in reply.
Popup {
    id: root

    property int editRow: -1

    width: 440
    modal: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    background: Rectangle {
        color: theme.surface
        radius: Math.min(10, theme.radius + 4)
        border.color: Qt.alpha(theme.muted, 0.35)
    }

    function openNew(isSend) {
        editRow = -1;
        nameField.text = isSend ? qsTr("New send") : qsTr("New receive");
        kindBox.currentIndex = isSend ? 0 : 1;
        formatBox.currentIndex = 0;
        dataArea.text = "";
        hotkeyBox.currentIndex = 0;
        repeatField.text = "0";
        replyBox.currentIndex = -1;
        open();
        nameField.selectAll();
        nameField.forceActiveFocus();
    }

    function openEdit(row) {
        const seq = backend.sequences.get(row);
        if (!seq || seq.row === undefined)
            return;
        editRow = row;
        nameField.text = seq.name;
        kindBox.currentIndex = seq.isSend ? 0 : 1;
        formatBox.currentIndex = seq.format;
        dataArea.text = seq.data;
        hotkeyBox.currentIndex = seq.hotkey > 0 ? seq.hotkey : 0;
        repeatField.text = String(seq.repeatIntervalMs);
        replyBox.currentIndex = -1;
        for (let i = 0; i < replyBox.count; ++i)
            if (replyBox.valueAt(i) === seq.replyToId)
                replyBox.currentIndex = i;
        open();
        nameField.forceActiveFocus();
    }

    function save() {
        const fields = {
            name: nameField.text === "" ? qsTr("Untitled") : nameField.text,
            isSend: kindBox.currentIndex === 0,
            format: formatBox.currentIndex,
            data: dataArea.text,
            hotkey: kindBox.currentIndex === 0 ? hotkeyBox.currentIndex : 0,
            repeatIntervalMs: kindBox.currentIndex === 0 ? parseInt(repeatField.text, 10) || 0 : 0,
            replyToId: kindBox.currentIndex === 1 && replyBox.currentIndex >= 0 ? replyBox.currentValue : -1
        };
        if (editRow >= 0)
            backend.sequences.update(editRow, fields);
        else
            backend.sequences.add(fields);
        close();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 10

        Label {
            text: root.editRow >= 0 ? qsTr("Edit sequence") : qsTr("New sequence")
            font.pixelSize: 14
            font.weight: Font.DemiBold
            color: theme.accent
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("Name")
                font.pixelSize: 13
                onAccepted: root.save()
            }
            ComboBox {
                id: kindBox
                Layout.preferredWidth: 130
                font.pixelSize: 12
                model: [qsTr("Send"), qsTr("Receive")]
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ComboBox {
                id: formatBox
                Layout.preferredWidth: 110
                font.pixelSize: 12
                model: [qsTr("Text"), qsTr("Hex"), qsTr("Dec")]
            }
            ComboBox {
                id: hotkeyBox
                Layout.preferredWidth: 96
                font.pixelSize: 12
                model: ["—", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"]
            }
            TextField {
                id: repeatField
                Layout.preferredWidth: 110
                visible: kindBox.currentIndex === 0
                placeholderText: qsTr("Repeat ms")
                font.pixelSize: 12
                font.family: win.monoFont
                ToolTip.visible: repeatField.hovered
                ToolTip.text: qsTr("Resend every N ms while active; 0 = once")
                ToolTip.delay: 500
            }
        }

        TextArea {
            id: dataArea
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            wrapMode: TextArea.WrapAnywhere
            font.family: win.monoFont
            font.pixelSize: 12
            color: theme.fg
            placeholderText: kindBox.currentIndex === 0
                             ? qsTr("Data to send — [0D] inserts a hex byte, [[ a literal [")
                             : qsTr("Pattern to watch for — ?? matches one unknown byte")
            background: Rectangle {
                color: theme.deep
                radius: Math.min(6, theme.radius + 2)
                border.color: dataArea.activeFocus ? Qt.alpha(theme.accent, 0.5) : Qt.alpha(theme.muted, 0.3)
            }
        }

        Label {
            Layout.fillWidth: true
            text: {
                const parsed = backend.parseError(dataArea.text, formatBox.currentIndex);
                if (parsed !== "")
                    return parsed;
                const bytes = backend.previewBytes(dataArea.text, formatBox.currentIndex);
                return bytes === "" ? "" : bytes + "  (" + bytes.split(" ").length + " bytes)";
            }
            color: backend.parseError(dataArea.text, formatBox.currentIndex) !== "" ? theme.red : theme.muted
            font.pixelSize: 11
            font.family: win.monoFont
            wrapMode: Text.WrapAnywhere
            visible: text !== ""
        }

        RowLayout {
            visible: kindBox.currentIndex === 1
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: qsTr("On match, send")
                color: theme.muted
                font.pixelSize: 12
            }
            ComboBox {
                id: replyBox
                Layout.fillWidth: true
                font.pixelSize: 12
                textRole: "label"
                valueRole: "value"
                model: {
                    const entries = [{ label: qsTr("(nothing)"), value: -1 }];
                    const seqs = backend.sequences;
                    for (let i = 0; i < seqs.count; ++i) {
                        const seq = seqs.get(i);
                        if (seq.isSend)
                            entries.push({ label: seq.name, value: seq.seqId });
                    }
                    return entries;
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 8
            Button {
                visible: root.editRow >= 0
                text: qsTr("Delete")
                onClicked: {
                    backend.sequences.remove(root.editRow);
                    root.close();
                }
            }
            Button {
                text: qsTr("Cancel")
                flat: true
                onClicked: root.close()
            }
            Button {
                text: qsTr("Save")
                highlighted: true
                onClicked: root.save()
            }
        }
    }
}
