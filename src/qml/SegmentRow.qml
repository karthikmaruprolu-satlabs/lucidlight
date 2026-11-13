import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// A row of pill buttons acting as one segmented control.
Item {
    id: root
    property alias model: repeater.model
    property int currentIndex: 0
    signal activated(int index)

    implicitWidth: row.implicitWidth
    implicitHeight: 30

    HoverHandler {
        id: hover
    }
    property bool hovered: hover.hovered

    Rectangle {
        anchors.fill: parent
        color: theme.surfaceAlt
        radius: Math.min(6, theme.radius + 2)
        border.color: Qt.alpha(theme.muted, 0.25)
    }

    Row {
        id: row
        anchors.fill: parent

        Repeater {
            id: repeater

            delegate: Item {
                required property int index
                required property string modelData
                width: label.implicitWidth + 24
                height: row.height

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 3
                    color: root.currentIndex === index ? Qt.alpha(theme.accent, 0.22) : "transparent"
                    radius: Math.min(4, theme.radius + 1)
                    border.color: root.currentIndex === index ? Qt.alpha(theme.accent, 0.55) : "transparent"
                }
                Label {
                    id: label
                    anchors.centerIn: parent
                    text: modelData
                    font.pixelSize: 12
                    color: root.currentIndex === index ? theme.accent : theme.muted
                    font.weight: root.currentIndex === index ? Font.DemiBold : Font.Normal
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: root.enabled
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.activated(index)
                }
            }
        }
    }
}
