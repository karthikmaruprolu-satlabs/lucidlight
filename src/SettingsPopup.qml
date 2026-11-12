import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Port configuration for channels A and B.
Popup {
    id: root

    property int channel: 0

    width: 420
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: theme.surface
        radius: Math.min(10, theme.radius + 4)
        border.color: Qt.alpha(theme.muted, 0.35)
    }

    function openFor(channel, anchorItem) {
        root.channel = channel;
        if (anchorItem) {
            const pos = anchorItem.mapToItem(root.parent, 0, anchorItem.height);
            root.x = Math.min(Math.max(8, pos.x - 8), root.parent.width - root.width - 8);
            root.y = Math.min(pos.y + 6, root.parent.height - root.height - 8);
        }
        open();
    }

    ColumnLayout {
        id: settingsColumn
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        Label {
            text: root.channel === 0 ? qsTr("Port A") : qsTr("Port B")
            font.pixelSize: 14
            font.weight: Font.DemiBold
            color: theme.accent
        }

        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 8
            Layout.fillWidth: true

            Label { text: qsTr("Device"); color: theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: deviceBox
                Layout.fillWidth: true
                editable: true
                font.pixelSize: 12
                font.family: win.monoFont
                model: backend.ports.ports()
                // Network channels are typed directly, Docklight-style.
                placeholderText: qsTr("TCP:host:port · SERVER:port · UDP:host:port")
                editText: root.channel === 0 ? backend.channelA.device : backend.channelB.device
                onActivated: applyDevice()
                onAccepted: applyDevice()

                function applyDevice() {
                    if (root.channel === 0)
                        backend.channelA.device = editText;
                    else
                        backend.channelB.device = editText;
                }
                popup: Popup {
                    y: deviceBox.height
                    width: deviceBox.width
                    height: Math.min(contentItem.implicitHeight + 12, 260)
                }
            }

            Label { text: qsTr("Baud rate"); color: theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: baudBox
                Layout.fillWidth: true
                editable: true
                font.pixelSize: 12
                font.family: win.monoFont
                model: [300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000, 4000000]
                onActivated: (index) => applyRate(currentText)
                onAccepted: applyRate(editText)

                function applyRate(value) {
                    const rate = parseInt(value, 10);
                    if (isNaN(rate))
                        return;
                    if (root.channel === 0)
                        backend.channelA.baud = rate;
                    else
                        backend.channelB.baud = rate;
                }
            }

            Label { text: qsTr("Data bits"); color: theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: dataBox
                Layout.fillWidth: true
                font.pixelSize: 12
                model: [5, 6, 7, 8]
                onActivated: (index) => setBits(model[index])

                function setBits(bits) {
                    if (root.channel === 0)
                        backend.channelA.dataBits = bits;
                    else
                        backend.channelB.dataBits = bits;
                }
            }

            Label { text: qsTr("Parity"); color: theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: parityBox
                Layout.fillWidth: true
                font.pixelSize: 12
                textRole: "label"
                valueRole: "value"
                model: [
                    { label: qsTr("None"), value: 0 },
                    { label: qsTr("Even"), value: 1 },
                    { label: qsTr("Odd"), value: 2 },
                    { label: qsTr("Mark"), value: 3 },
                    { label: qsTr("Space"), value: 4 }
                ]
                onActivated: (index) => setParity(model[index].value)

                function setParity(parity) {
                    if (root.channel === 0)
                        backend.channelA.parity = parity;
                    else
                        backend.channelB.parity = parity;
                }
            }

            Label { text: qsTr("Stop bits"); color: theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: stopBox
                Layout.fillWidth: true
                font.pixelSize: 12
                model: [1, 2]
                onActivated: (index) => setStops(model[index])

                function setStops(bits) {
                    if (root.channel === 0)
                        backend.channelA.stopBits = bits;
                    else
                        backend.channelB.stopBits = bits;
                }
            }

            Label { text: qsTr("Flow control"); color: theme.muted; font.pixelSize: 12 }
            ComboBox {
                id: flowBox
                Layout.fillWidth: true
                font.pixelSize: 12
                textRole: "label"
                valueRole: "value"
                model: [
                    { label: qsTr("None"), value: 0 },
                    { label: qsTr("RTS/CTS"), value: 1 },
                    { label: qsTr("Xon/Xoff"), value: 2 }
                ]
                onActivated: (index) => setFlow(model[index].value)

                function setFlow(flow) {
                    if (root.channel === 0)
                        backend.channelA.flow = flow;
                    else
                        backend.channelB.flow = flow;
                }
            }
        }

        // Sync the boxes with the settings each time the popup opens.
        function syncFromSettings() {
            const settings = root.channel === 0 ? backend.channelA : backend.channelB;
            deviceBox.model = backend.ports.ports();
            deviceBox.editText = settings.device;
            baudBox.editText = String(settings.baud);
            dataBox.currentIndex = [5, 6, 7, 8].indexOf(settings.dataBits);
            if (dataBox.currentIndex < 0)
                dataBox.currentIndex = 3;
            for (let i = 0; i < parityBox.model.length; ++i)
                if (parityBox.model[i].value === settings.parity)
                    parityBox.currentIndex = i;
            stopBox.currentIndex = settings.stopBits === 2 ? 1 : 0;
            for (let j = 0; j < flowBox.model.length; ++j)
                if (flowBox.model[j].value === settings.flow)
                    flowBox.currentIndex = j;
        }
    }

    onOpened: settingsColumn.syncFromSettings()
}
