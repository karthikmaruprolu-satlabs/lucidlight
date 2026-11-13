import QtQuick
import QtQuick.Controls

// A transient message at the bottom of the window.
Rectangle {
    id: root
    width: Math.max(160, message.implicitWidth + 40)
    height: 34
    radius: Math.min(8, theme.radius + 4)
    color: theme.surface
    border.color: Qt.alpha(theme.accent, 0.4)
    opacity: 0

    function show(text) {
        message.text = text;
        opacity = 1;
        hideTimer.restart();
    }

    Behavior on opacity {
        NumberAnimation { duration: 180 }
    }

    Timer {
        id: hideTimer
        interval: 2600
        onTriggered: root.opacity = 0
    }

    Label {
        id: message
        anchors.centerIn: parent
        color: theme.bright
        font.pixelSize: 12
    }
}
