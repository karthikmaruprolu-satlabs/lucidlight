import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win

    width: 1120
    height: 720
    minimumWidth: 760
    minimumHeight: 500
    visible: true
    title: {
        const name = qsTr("Lucidlight");
        const file = backend.projectPath !== "" ? backend.projectPath.split("/").pop()
                                                : qsTr("Untitled");
        return backend.dirty ? name + " — " + file + " *" : name + " — " + file;
    }
    color: theme.bg

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    readonly property string monoFont: "JetBrains Mono Nerd Font"
    property bool helpVisible: false
    property bool searchVisible: false
    property bool forceClose: false

    // Sequence hotkeys, the F1..F12 send bar of Docklight.
    Shortcut { sequence: "F1";  context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(1) }
    Shortcut { sequence: "F2";  context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(2) }
    Shortcut { sequence: "F3";  context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(3) }
    Shortcut { sequence: "F4";  context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(4) }
    Shortcut { sequence: "F5";  context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(5) }
    Shortcut { sequence: "F6";  context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(6) }
    Shortcut { sequence: "F7";  context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(7) }
    Shortcut { sequence: "F8";  context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(8) }
    Shortcut { sequence: "F9";  context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(9) }
    Shortcut { sequence: "F10"; context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(10) }
    Shortcut { sequence: "F11"; context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(11) }
    Shortcut { sequence: "F12"; context: Qt.ApplicationShortcut; onActivated: backend.sendByHotkey(12) }

    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !focusInTextField(); onActivated: backend.toggleComm() }
    Shortcut { sequence: "Ctrl+K"; context: Qt.ApplicationShortcut; onActivated: backend.clearLog() }
    Shortcut { sequence: "Ctrl+F"; context: Qt.ApplicationShortcut; onActivated: {
        searchVisible = !searchVisible;
        if (searchVisible) searchField.forceActiveFocus();
    } }
    Shortcut { sequence: "Ctrl+B"; context: Qt.ApplicationShortcut; onActivated: sidebar.visible = !sidebar.visible }
    Shortcut { sequence: "Ctrl+S"; context: Qt.ApplicationShortcut; onActivated: saveProject() }
    Shortcut { sequence: "Ctrl+O"; context: Qt.ApplicationShortcut; onActivated: openProject() }
    Shortcut { sequence: "Ctrl+R"; context: Qt.ApplicationShortcut; onActivated: {
        bottomBar.currentIndex = 1;
        scriptPanel.runScript();
    } }
    Shortcut { sequence: "Ctrl+Shift+S"; context: Qt.ApplicationShortcut; onActivated: saveLog() }
    Shortcut { sequence: "Ctrl+P"; context: Qt.ApplicationShortcut; onActivated: printLog() }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !focusInTextField(); onActivated: helpVisible = !helpVisible }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; onActivated: win.close() }
    // Docklight function-key map: F5/F6 communication, F2/F3 logging,
    // Shift+F5/F6/F7 script control, Ctrl+T save script.
    Shortcut { sequence: "F5"; enabled: !focusInTextField(); onActivated: backend.toggleComm() }
    Shortcut { sequence: "F6"; enabled: !focusInTextField(); onActivated: backend.toggleComm() }
    Shortcut { sequence: "F2"; enabled: !focusInTextField(); onActivated: if (!backend.fileLogging) backend.startFileLogging() }
    Shortcut { sequence: "F3"; enabled: !focusInTextField(); onActivated: backend.stopFileLogging() }
    Shortcut { sequence: "Shift+F5"; onActivated: { bottomBar.currentIndex = 1; scriptPanel.runScript(); } }
    Shortcut { sequence: "Shift+F6"; onActivated: backend.scripts.stop() }
    Shortcut { sequence: "Shift+F7"; onActivated: bottomBar.currentIndex = 1 }
    Shortcut { sequence: "Ctrl+T"; onActivated: saveScript() }

    // True when the keyboard focus sits in a text entry, so plain keys must
    // type instead of triggering shortcuts.
    function focusInTextField() {
        let item = win.activeFocusItem;
        while (item) {
            if (item instanceof TextInput || item instanceof TextEdit)
                return true;
            if (item instanceof ListView && item.objectName === "commView"
                && item.activeFocus && backend.keyboardConsole)
                return true;   // keyboard console owns the keystrokes
            item = item.parent;
        }
        return false;
    }

    function saveProject() {
        if (backend.projectPath !== "") {
            backend.saveProjectTo(backend.projectPath);
        } else {
            pickerFlows.openSave = false;
            picker.saveFile(qsTr("Save project"), qsTr("Lucidlight project"), "*.llproj", qsTr("untitled.llproj"));
        }
    }

    function openProject() {
        pickerFlows.openSave = true;
        picker.openFile(qsTr("Open project"), qsTr("Lucidlight project"), "*.llproj");
    }

    // Ctrl+T: save the script text to a .js file.
    function saveScript() {
        pickerFlows.openSave = true;
        pickerFlows.kind = "script";
        picker.saveFile(qsTr("Save script"), qsTr("JavaScript"), "*.js", "script.js");
    }

    function saveLog() {
        pickerFlows.openSave = false;
        pickerFlows.kind = "log";
        picker.saveFile(qsTr("Save communication log"), qsTr("Log file"), "*.log", "lucidlight.log");
    }

    function printLog() {
        pickerFlows.openSave = false;
        pickerFlows.kind = "print";
        picker.saveFile(qsTr("Print communication window (PDF)"), qsTr("PDF"), "*.pdf",
                        "lucidlight.pdf");
    }

    // Harness hook: open one of the dialogs by name.
    function openFor(name) {
        if (name === "settings")
            settingsPopup.openFor(0);
        else if (name === "sequence")
            sequenceDialog.openNew(true);
        else if (name === "editor")
            sequenceDialog.openEdit(0);
        else if (name === "help")
            win.helpVisible = true;
        else if (name === "params") {
            // Open the Send Sequence Parameter dialog for the first send
            // sequence that has wildcards.
            const count = backend.sequences.count;
            for (let r = 0; r < count; ++r) {
                const parsed = backend.sequenceWildcardAreas(r);
                if (parsed.length > 0) {
                    parameterDialog.openFor(r, parsed);
                    break;
                }
            }
        }
    }

    // Routes portal picker results to the right action.
    QtObject {
        id: pickerFlows
        property bool openSave: true
        // Which artifact the picker is saving: project or script.
        property string kind: "project"
    }

    onClosing: (close) => {
        if (forceClose || !backend.dirty) {
            close.accepted = true;
            return;
        }
        close.accepted = false;
        quitDialog.open();
    }

    Connections {
        target: backend.picker
        function onFileOpened(path) {
            if (pickerFlows.openSave && path !== "")
                backend.openProjectFrom(path);
        }
        function onFileSaved(path) {
            if (pickerFlows.openSave && pickerFlows.kind === "script") {
                if (path !== "")
                    backend.saveScriptTo(path);
            } else if (!pickerFlows.openSave && pickerFlows.kind === "print") {
                if (path !== "")
                    backend.printLogTo(path);
            } else if (pickerFlows.openSave) {
                if (path !== "") {
                    backend.saveProjectTo(path);
                    if (quitDialog.opened) {
                        quitDialog.close();
                        win.forceClose = true;
                        win.close();
                    }
                }
            } else if (path !== "") {
                backend.saveLogTo(path);
            }
        }
    }

    // ------------------------------------------------------------------
    // Layout
    // ------------------------------------------------------------------

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- toolbar ---------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            spacing: 8

            Button {
                id: startButton
                Layout.preferredWidth: 108
                Layout.preferredHeight: 36
                checked: backend.running
                contentItem: RowLayout {
                    spacing: 8
                    Text {
                        text: backend.running ? "■" : "▶"
                        font.pixelSize: 13
                        color: backend.running ? theme.red : theme.green
                    }
                    Label {
                        text: backend.running ? qsTr("Stop") : qsTr("Start")
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        color: startButton.hovered ? theme.fg : theme.bright
                    }
                }
                background: Rectangle {
                    color: backend.running ? Qt.alpha(theme.red, 0.12) : Qt.alpha(theme.green, 0.12)
                    border.color: backend.running ? Qt.alpha(theme.red, 0.5) : Qt.alpha(theme.green, 0.5)
                    border.width: 1
                    radius: Math.min(8, theme.radius + 4)
                }
                onClicked: backend.toggleComm()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Start or stop communication (Space)")
                ToolTip.delay: 600
            }

            SegmentRow {
                id: modeRow
                model: [qsTr("Single"), qsTr("Redirect"), qsTr("Monitor")]
                currentIndex: backend.comm.mode
                enabled: !backend.running
                onActivated: (index) => backend.comm.mode = index
                ToolTip.visible: modeRow.hovered
                ToolTip.text: qsTr("Single: one port. Redirect: send on A, receive on B. Monitor: tap both sides.")
                ToolTip.delay: 600
            }

            PortChip {
                id: chipA
                label: "A"
                settings: backend.channelA
                onClicked: settingsPopup.openFor(0, chipA)
            }
            PortChip {
                id: chipB
                label: "B"
                settings: backend.channelB
                visible: backend.comm.mode !== 0
                onClicked: settingsPopup.openFor(1, chipB)
            }

            Item { Layout.fillWidth: true }

            // Docklight contents filter: All / Ch 1 / Ch 2 / Comments.
            SegmentRow {
                id: filterRow
                model: [qsTr("All"), qsTr("Ch 1"), qsTr("Ch 2"), qsTr("Comments")]
                currentIndex: backend.log.channelFilter
                onActivated: (index) => backend.log.channelFilter = index
                ToolTip.visible: filterRow.hovered
                ToolTip.text: qsTr("Contents filter — show everything, one channel, or only comments")
                ToolTip.delay: 600
            }

            Item { Layout.fillWidth: true }

            Button {
                id: searchButton
                Layout.preferredHeight: 32
                Layout.preferredWidth: 36
                flat: true
                text: "⌕"
                font.pixelSize: 16
                onClicked: {
                    win.searchVisible = !win.searchVisible;
                    if (win.searchVisible)
                        searchField.forceActiveFocus();
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Search in the communication window (Ctrl+F)")
                ToolTip.delay: 600
            }
            ToolButton {
                id: tsButton
                checkable: true
                checked: backend.log.showTimestamps
                text: "◷"
                font.pixelSize: 14
                onClicked: backend.log.showTimestamps = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Timestamps")
                ToolTip.delay: 600
            }
            // Docklight's "additional date/time stamp": a full stamp into
            // the communication window every N seconds, 0 = off.
            ToolButton {
                id: clockStampButton
                text: "⏲"
                font.pixelSize: 14
                onClicked: clockStampMenu.popup(clockStampButton, 0, clockStampButton.height)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Clock stamp every N seconds (currently: %1)").arg(
                    backend.clockStampInterval > 0 ? qsTr("%1 s").arg(backend.clockStampInterval)
                                                   : qsTr("off"))
                ToolTip.delay: 600
                Menu {
                    id: clockStampMenu
                    MenuItem {
                        text: qsTr("Off")
                        onTriggered: backend.clockStampInterval = 0
                    }
                    MenuItem {
                        text: qsTr("Every 1 second")
                        onTriggered: backend.clockStampInterval = 1
                    }
                    MenuItem {
                        text: qsTr("Every 5 seconds")
                        onTriggered: backend.clockStampInterval = 5
                    }
                    MenuItem {
                        text: qsTr("Every 10 seconds")
                        onTriggered: backend.clockStampInterval = 10
                    }
                    MenuItem {
                        text: qsTr("Every 60 seconds")
                        onTriggered: backend.clockStampInterval = 60
                    }
                }
            }
            ToolButton {
                id: pauseStampButton
                checkable: true
                checked: backend.log.pauseStampMs > 0
                text: "⏱"
                font.pixelSize: 14
                onClicked: backend.log.pauseStampMs = checked ? 1000 : 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Pause detection — after 1 s of silence the delta clock restarts")
                ToolTip.delay: 600
            }
            ToolButton {
                id: ctrlNamesButton
                checkable: true
                checked: backend.log.showControlNames
                text: "⏎"
                font.pixelSize: 14
                onClicked: backend.log.showControlNames = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Show control characters as <CR>, <LF>, <ESC> … in the ASCII view")
                ToolTip.delay: 600
            }
            ToolButton {
                id: deltaButton
                checkable: true
                checked: backend.log.showDelta
                text: "∆"
                font.pixelSize: 14
                onClicked: backend.log.showDelta = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Time since the previous line")
                ToolTip.delay: 600
            }
            ToolButton {
                id: freezeButton
                checkable: true
                checked: backend.log.frozen
                text: checked ? "▶" : "❚❚"
                font.pixelSize: 12
                onClicked: backend.log.frozen = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Freeze the view; data keeps flowing underneath")
                ToolTip.delay: 600
            }
            ToolButton {
                id: clearButton
                text: "✕"
                font.pixelSize: 14
                onClicked: backend.clearLog()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Clear the window (Ctrl+K)")
                ToolTip.delay: 600
            }
            ToolButton {
                id: saveLogButton
                text: "⤓"
                font.pixelSize: 14
                onClicked: saveLog()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Save the log (Ctrl+Shift+S)")
                ToolTip.delay: 600
            }
            ToolButton {
                id: menuButton
                text: "☰"
                font.pixelSize: 15
                onClicked: projectMenu.popup(menuButton, 0, menuButton.height + 4)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Project")
                ToolTip.delay: 600
            }
        }

        // ---- search row --------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            visible: win.searchVisible
            spacing: 8
            TextField {
                id: searchField
                Layout.preferredWidth: 320
                placeholderText: qsTr("Search in data…")
                font.pixelSize: 13
                onTextChanged: {
                    backend.log.setSearchText(text);
                    backend.log.currentFindRow = -1;
                    findNextButton.enabled = text !== "";
                }
                Keys.onEscapePressed: win.searchVisible = false
                Keys.onReturnPressed: (event) => findNextButton.doFind()
                Keys.onEnterPressed: (event) => findNextButton.doFind()
            }
            Button {
                id: findNextButton
                flat: true
                text: qsTr("Next ⏎")
                font.pixelSize: 12
                enabled: searchField.text !== ""
                function doFind() {
                    const row = backend.log.findNext(searchField.text, backend.log.currentFindRow);
                    if (row >= 0) {
                        backend.log.currentFindRow = row;
                        commView.positionViewAtIndex(row, ListView.Contain);
                        findStatusLabel.text = "";
                    } else {
                        findStatusLabel.text = qsTr("No match");
                    }
                }
                onClicked: doFind()
            }
            Label {
                id: findStatusLabel
                color: theme.muted
                font.pixelSize: 12
            }
        }

        // ---- body ----------------------------------------------------------
        RowLayout {
            id: bodyRow
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            SequencePanel {
                id: sidebar
                Layout.fillHeight: true
                Layout.preferredWidth: 250
                onRequestEdit: (row) => sequenceDialog.openEdit(row)
                onRequestAdd: (isSend) => sequenceDialog.openNew(isSend)
            }

            ColumnLayout {
                id: bodyCol
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 8
                spacing: 8

                // The communication window.
                Rectangle {
                    id: commRect
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: theme.deep
                    radius: Math.min(8, theme.radius + 4)
                    border.color: Qt.alpha(theme.muted, 0.25)

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        // Docklight's representation tabs: ASCII/HEX/DEC/Binary,
                        // cycled with Ctrl+Tab like the original.
                        SegmentRow {
                            id: reprRow
                            Layout.margins: 6
                            Layout.alignment: Qt.AlignHCenter
                            model: [qsTr("ASCII"), qsTr("HEX"), qsTr("DEC"), qsTr("Binary")]
                            currentIndex: backend.log.displayFormat
                            onActivated: (index) => backend.log.displayFormat = index
                        }

                        ListView {
                            id: commView
                            objectName: "commView"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: backend.log
                            reuseItems: false
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}

                            // Docklight's Keyboard Console: with the tool on,
                            // clicking into the window and typing transmits
                            // directly through the port.
                            focus: backend.keyboardConsole
                            activeFocusOnTab: backend.keyboardConsole
                            TapHandler {
                                enabled: backend.keyboardConsole
                                onTapped: commView.forceActiveFocus()
                            }
                            Keys.onPressed: (event) => {
                                if (!backend.keyboardConsole)
                                    return;
                                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                    backend.sendText("\r\n", 0);
                                    event.accepted = true;
                                } else if (event.key === Qt.Key_Escape) {
                                    backend.sendText("\u001B", 0);
                                    event.accepted = true;
                                } else if (event.key === Qt.Key_V && event.modifiers & Qt.ControlModifier) {
                                    let clip = backend.clipboardText();
                                    if (clip.length > 1024)
                                        clip = clip.substring(0, 1024);
                                    backend.sendText(clip, 0);
                                    event.accepted = true;
                                } else if (event.text.length === 1 && !event.modifiers) {
                                    backend.sendText(event.text, 0);
                                    event.accepted = true;
                                }
                            }

                            property bool atEnd: true
                            onContentYChanged: atEnd = atYEnd
                            onCountChanged: if (atEnd) positionViewAtEnd()

                            delegate: Item {
                                id: logRowWrapper
                                // Required properties must sit on the delegate
                                // root: nested items cannot see model roles.
                                required property int index
                                required property int entryIndex
                                required property string clock
                                required property string delta
                                required property int direction
                                required property int channel
                                required property string text
                                required property string rawHex
                                required property bool highlight
                                required property bool comment
                                required property bool searchHit
                                width: commView.width
                                implicitHeight: rowDelegate.implicitHeight
                                Rectangle {
                                    anchors.fill: parent
                                    color: Qt.alpha(theme.accent, 0.22)
                                    visible: logRowWrapper.index === backend.log.currentFindRow
                                }
                                CommRow {
                                    id: rowDelegate
                                    anchors.fill: parent
                                    entryIndex: logRowWrapper.entryIndex
                                    index: logRowWrapper.index
                                    clock: logRowWrapper.clock
                                    delta: logRowWrapper.delta
                                    direction: logRowWrapper.direction
                                    channel: logRowWrapper.channel
                                    text: logRowWrapper.text
                                    rawHex: logRowWrapper.rawHex
                                    highlight: logRowWrapper.highlight
                                    comment: logRowWrapper.comment
                                    searchHit: logRowWrapper.searchHit
                                    onContextMenuRequested: (row) => rowMenu.popupAt(row)
                                }
                            }

                            // The empty-state hint.
                            Column {
                                anchors.centerIn: parent
                                visible: backend.log.count === 0
                                spacing: 6
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: qsTr("The communication window is empty")
                                    color: theme.muted
                                    font.pixelSize: 13
                                }
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: qsTr("Press Space to start, then type below and hit Enter")
                                    color: Qt.alpha(theme.muted, 0.7)
                                    font.pixelSize: 12
                                }
                            }

                            // A thin strip that says the view is frozen.
                            Rectangle {
                                anchors.top: parent.top
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 22
                                visible: backend.log.frozen
                                color: Qt.alpha(theme.yellow, 0.12)
                                Label {
                                    anchors.centerIn: parent
                                    text: qsTr("Frozen — press the ❚❚ toolbar button to resume")
                                    color: theme.yellow
                                    font.pixelSize: 11
                                }
                            }
                        }
                    }
                }

                // ---- bottom panel: send / script -----------------------------
                TabBar {
                    id: bottomBar
                    objectName: "bottomBar"
                    Layout.fillWidth: true
                    currentIndex: 0
                    background: null

                    TabButton {
                        text: qsTr("Send")
                        font.pixelSize: 12
                    }
                    TabButton {
                        text: qsTr("Script")
                        font.pixelSize: 12
                    }
                }

                StackLayout {
                    id: bottomStack
                    Layout.fillWidth: true
                    // StackLayout defaults to expanding vertically, which
                    // would starve the communication window above it.
                    Layout.fillHeight: false
                    currentIndex: bottomBar.currentIndex

                    SendPanel {
                        id: sendPanel
                    }
                    ScriptPanel {
                        id: scriptPanel
                    }
                }
            }
        }

        // ---- status bar -----------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            color: theme.surfaceAlt

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 14

                Rectangle {
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    radius: 4
                    color: backend.running ? theme.green : theme.muted
                }
                Label {
                    text: backend.commStateText
                    color: theme.muted
                    font.pixelSize: 11
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: {
                        const modes = [qsTr("Single port"), qsTr("Redirect A→B"), qsTr("Monitor tap")];
                        return modes[backend.comm.mode];
                    }
                    color: theme.muted
                    font.pixelSize: 11
                }
                Label {
                    visible: backend.projectPath !== ""
                    text: backend.projectPath.split("/").pop() + (backend.dirty ? " *" : "")
                    color: backend.dirty ? theme.yellow : theme.muted
                    font.pixelSize: 11
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Overlays
    // ------------------------------------------------------------------

    SettingsPopup {
        id: settingsPopup
    }

    SequenceDialog {
        id: sequenceDialog
    }

    // Docklight's "Send Sequence Parameter" dialog: pops up when a send
    // sequence with wildcard parameters is started from the UI.
    ParameterDialog {
        id: parameterDialog
    }

    Connections {
        target: backend
        function onParameterRequest(row, minLengths) {
            parameterDialog.openFor(row, minLengths);
        }
    }

    HelpOverlay {
        id: helpOverlay
        visible: win.helpVisible
        onClosed: win.helpVisible = false
    }

    Menu {
        id: projectMenu
        MenuItem {
            text: qsTr("New project")
            onTriggered: backend.newProject()
        }
        MenuItem {
            text: qsTr("Open…") + "  (Ctrl+O)"
            onTriggered: openProject()
        }
        MenuItem {
            text: qsTr("Save") + "  (Ctrl+S)"
            onTriggered: saveProject()
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Save communication log…") + "  (Ctrl+Shift+S)"
            onTriggered: saveLog()
        }
        MenuItem {
            text: qsTr("Print communication window…") + "  (Ctrl+P)"
            onTriggered: printLog()
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Keyboard Console On") + "  (Ctrl+T)"
            checkable: true
            checked: backend.keyboardConsole
            enabled: backend.commStateText.indexOf("single") >= 0
            onTriggered: backend.keyboardConsole = checked
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Shortcuts  (?)")
            onTriggered: win.helpVisible = true
        }
        MenuItem {
            text: qsTr("Quit") + "  (Ctrl+Q)"
            onTriggered: win.close()
        }
    }

    Menu {
        id: rowMenu
        property int row: -1
        function popupAt(row) {
            rowMenu.row = row;
            popup();
        }
        MenuItem {
            text: qsTr("Copy data as text")
            onTriggered: backend.copyToClipboard(backend.log.textAt(rowMenu.row))
        }
        MenuItem {
            text: qsTr("Copy data as hex")
            onTriggered: backend.copyToClipboard(backend.log.hexAt(rowMenu.row))
        }
    }

    Popup {
        id: quitDialog
        anchors.centerIn: parent
        width: 380
        modal: true
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            color: theme.surface
            radius: Math.min(10, theme.radius + 4)
            border.color: Qt.alpha(theme.muted, 0.3)
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 12
            Label {
                text: qsTr("Unsaved changes")
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("The project has changes that are not saved. Save before quitting?")
                wrapMode: Text.Wrap
                color: theme.fg
                font.pixelSize: 13
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                Button {
                    text: qsTr("Cancel")
                    flat: true
                    onClicked: quitDialog.close()
                }
                Button {
                    text: qsTr("Discard")
                    onClicked: {
                        quitDialog.close();
                        win.forceClose = true;
                        win.close();
                    }
                }
                Button {
                    text: qsTr("Save")
                    highlighted: true
                    onClicked: {
                        quitDialog.close();
                        saveProject();
                    }
                }
            }
        }
    }

    // Blocking script dialogs: DL.MsgBox2 and DL.InputBox2 arrive here while
    // the script worker parks until the answer flows back.
    Popup {
        id: scriptDialog
        anchors.centerIn: parent
        width: 420
        modal: true
        closePolicy: Popup.NoAutoClose
        background: Rectangle {
            color: theme.surface
            radius: Math.min(10, theme.radius + 4)
            border.color: Qt.alpha(theme.muted, 0.3)
        }
        property int dlgButtons: 1
        property bool wantsInput: false
        property string titleText: "Script"
        property string promptText: ""
        property string defaultText: ""
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 12
            Label {
                text: scriptDialog.titleText
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: scriptDialog.promptText
                wrapMode: Text.Wrap
                color: theme.fg
                font.pixelSize: 13
            }
            TextField {
                id: scriptDialogInput
                Layout.fillWidth: true
                visible: scriptDialog.wantsInput
                text: scriptDialog.defaultText
                onAccepted: scriptDialog.answer(1)
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                Button {
                    visible: (scriptDialog.dlgButtons & 2) !== 0
                    text: qsTr("Cancel")
                    flat: true
                    onClicked: scriptDialog.answer(2)
                }
                Button {
                    visible: (scriptDialog.dlgButtons & 4) !== 0
                    text: qsTr("Abort")
                    flat: true
                    onClicked: scriptDialog.answer(3)
                }
                Button {
                    visible: (scriptDialog.dlgButtons & 8) !== 0
                    text: qsTr("Retry")
                    flat: true
                    onClicked: scriptDialog.answer(4)
                }
                Button {
                    visible: (scriptDialog.dlgButtons & 16) !== 0
                    text: qsTr("Ignore")
                    flat: true
                    onClicked: scriptDialog.answer(5)
                }
                Button {
                    visible: (scriptDialog.dlgButtons & 32) !== 0
                    text: qsTr("Yes")
                    flat: true
                    onClicked: scriptDialog.answer(6)
                }
                Button {
                    visible: (scriptDialog.dlgButtons & 64) !== 0
                    text: qsTr("No")
                    flat: true
                    onClicked: scriptDialog.answer(7)
                }
                Button {
                    visible: (scriptDialog.dlgButtons & 1) !== 0
                    text: qsTr("OK")
                    highlighted: true
                    onClicked: scriptDialog.answer(1)
                }
            }
        }
        // Docklight MsgBox2/InputBox2 button bit assignments.
        function answer(value) {
            backend.answerScriptDialog(value, wantsInput ? scriptDialogInput.text : "");
            close();
        }
    }

    Toast {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 42
        anchors.horizontalCenter: parent.horizontalCenter
    }

    Connections {
        target: backend
        function onToast(text) { toast.show(text) }
        function onScriptDialogRequested(prompt, buttons, title, input, defaultText) {
            scriptDialog.promptText = prompt;
            scriptDialog.dlgButtons = buttons;
            scriptDialog.titleText = title;
            scriptDialog.wantsInput = input;
            scriptDialog.defaultText = defaultText;
            scriptDialogInput.text = defaultText;
            scriptDialog.open();
        }
    }

    Component.onCompleted: {
        // A first-run hint sequence so the list is not empty.
        if (backend.sequences.count === 0) {
            backend.sequences.add({ name: "Greet", isSend: true, format: 0, data: "Hello [0D][0A]", hotkey: 1 });
            backend.sequences.add({ name: "CR", isSend: true, format: 0, data: "[0D]", hotkey: 2 });
            backend.sequences.add({ name: "Ack ping", isSend: false, format: 0, data: "PING", hotkey: 0, replyToId: -1 });
        }
    }
}
