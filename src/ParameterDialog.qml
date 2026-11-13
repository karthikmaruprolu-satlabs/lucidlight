import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Docklight's "Send Sequence Parameter" dialog: one input field per wildcard
// area of the sequence about to be sent. Shown when a send sequence with
// wildcard parameters is started from the sequence list, hotkey or Space key.
Popup {
    id: root

    property int sendRow: -1
    property var minLengths: []

    width: 380
    height: implicitContentHeight + 16
    modal: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    background: Rectangle {
        color: theme.surface
        radius: Math.min(10, theme.radius + 4)
        border.color: Qt.alpha(theme.muted, 0.35)
    }

    function openFor(row, minLens) {
        sendRow = row;
        minLengths = minLens;
        paramsModel.clear();
        for (let i = 0; i < minLens.length; ++i)
            paramsModel.append({ "label": qsTr("Parameter %1").arg(i + 1) });
        rep.model = paramsModel;
        open();
        if (rep.count > 0 && rep.itemAt(0))
            rep.itemAt(0).field.forceActiveFocus();
    }

    function accept() {
        const values = [];
        for (let i = 0; i < rep.count; ++i)
            values.push(rep.itemAt(i).field.text);
        close();
        if (sendRow >= 0)
            backend.sendSequenceWithParams(sendRow, values);
        sendRow = -1;
    }

    ListModel { id: paramsModel }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        Label {
            text: qsTr("Send sequence parameters")
            color: theme.text
            font.bold: true
        }
        Label {
            text: qsTr("One value per wildcard area. Space separates single-character values in HEX/DEC/BIN.")
            color: theme.muted
            font.pixelSize: 11
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Repeater {
            id: rep
            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                property alias field: paramField
                Label {
                    text: model.label
                    color: theme.muted
                    font.pixelSize: 11
                }
                TextField {
                    id: paramField
                    Layout.fillWidth: true
                    color: theme.text
                    placeholderText: root.minLengths[index] > 1
                        ? qsTr("%1 characters").arg(root.minLengths[index]) : ""
                    onAccepted: root.accept()
                }
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            Button {
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            Button {
                text: qsTr("Send")
                highlighted: true
                onClicked: root.accept()
            }
        }
    }
}
