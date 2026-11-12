import QtQuick
import QtQuick.Controls

// One row of the communication window: timestamp, direction badge, data.
// Colors follow the meaning: RX reads green, TX reads blue, comments are
// italic yellow, and a receive-trigger match tints the row.
Item {
    id: root

    required property int entryIndex
    required property int index
    required property string clock
    required property string delta
    required property int direction
    required property int channel
    required property string text
    required property string rawHex
    required property bool highlight
    required property bool comment
    required property bool searchHit

    signal contextMenuRequested(int row)

    implicitHeight: comment ? 24 : 22
    z: searchHit ? 1 : 0

    Rectangle {
        anchors.fill: parent
        color: {
            if (root.searchHit)
                return Qt.alpha(theme.yellow, 0.16);
            if (root.highlight)
                return Qt.alpha(theme.accent, 0.14);
            if (rowMouse.containsMouse)
                return Qt.alpha(theme.muted, 0.08);
            return "transparent";
        }
    }

    MouseArea {
        id: rowMouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.RightButton
        onClicked: (mouse) => root.contextMenuRequested(root.index)
    }

    Row {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 10

        Label {
            id: timeLabel
            width: backend.log.showTimestamps ? 96 : 0
            visible: backend.log.showTimestamps
            anchors.verticalCenter: parent.verticalCenter
            text: root.clock
            color: theme.muted
            font.family: win.monoFont
            font.pixelSize: 11
        }
        Label {
            id: deltaLabel
            width: backend.log.showDelta ? 84 : 0
            visible: backend.log.showDelta
            anchors.verticalCenter: parent.verticalCenter
            text: "+" + root.delta
            color: Qt.alpha(theme.muted, 0.75)
            font.family: win.monoFont
            font.pixelSize: 11
        }
        Rectangle {
            id: badgeRect
            width: root.comment ? 0 : 42
            height: 16
            visible: !root.comment
            anchors.verticalCenter: parent.verticalCenter
            radius: Math.min(3, theme.radius + 1)
            color: {
                if (root.direction === 0)
                    return Qt.alpha(theme.green, 0.15);
                if (root.direction === 1)
                    return Qt.alpha(theme.blue, 0.18);
                return "transparent";
            }
            Label {
                anchors.centerIn: parent
                text: {
                    if (root.direction === 0)
                        return "RX" + (root.channel >= 0 ? String.fromCharCode(65 + root.channel) : "");
                    if (root.direction === 1)
                        return "TX" + (root.channel >= 0 ? String.fromCharCode(65 + root.channel) : "");
                    return "·";
                }
                color: root.direction === 0 ? theme.green : root.direction === 1 ? theme.blue : theme.muted
                font.family: win.monoFont
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }
        }
        Label {
            width: parent.width - timeLabel.width - deltaLabel.width - badgeRect.width - parent.spacing * 3
            anchors.verticalCenter: parent.verticalCenter
            text: root.comment ? "◆ " + root.text : root.text
            color: {
                if (root.comment)
                    return theme.yellow;
                if (root.direction === 0)
                    return Qt.lighter(theme.green, 1.15);
                if (root.direction === 1)
                    return Qt.lighter(theme.blue, 1.2);
                return theme.fg;
            }
            font.family: win.monoFont
            font.italic: root.comment
            font.pixelSize: 13
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
    }
}
