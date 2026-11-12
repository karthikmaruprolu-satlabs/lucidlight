import QtQuick
import QtQuick.Controls

// A toolbar chip summarizing one port: letter, device, baud. Click opens the
// settings popup for that channel.
Item {
    id: root
    property string label: "A"
    property QtObject settings
    signal clicked()

    width: chip.implicitWidth + 16
    height: 32

    Rectangle {
        id: chip
        anchors.fill: parent
        radius: Math.min(6, theme.radius + 2)
        color: chipMouse.containsMouse ? theme.surface : theme.surfaceAlt
        border.color: Qt.alpha(theme.muted, 0.3)

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 8
            spacing: 6

            Rectangle {
                width: 18
                height: 18
                anchors.verticalCenter: parent.verticalCenter
                radius: 4
                color: Qt.alpha(theme.accent, 0.2)
                Label {
                    anchors.centerIn: parent
                    text: root.label
                    color: theme.accent
                    font.pixelSize: 10
                    font.weight: Font.Bold
                }
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    const device = root.settings && root.settings.device !== "" ? root.settings.device : qsTr("no port");
                    return device + (root.settings && root.settings.baud ? " · " + root.settings.baud : "");
                }
                color: theme.fg
                font.pixelSize: 11
                font.family: win.monoFont
            }
        }
        MouseArea {
            id: chipMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.clicked()
        }
    }
}
