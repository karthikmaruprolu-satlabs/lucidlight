#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QFileSystemWatcher>

#include "channelsettings.h"
#include "commlog.h"
#include "commmanager.h"
#include "filelogger.h"
#include "dlscript.h"
#include "portenumerator.h"
#include "portalpicker.h"
#include "sequencemodel.h"
#include "sequencematcher.h"

// The application's state and work, exposed to QML as `backend`. Owns every
// subsystem and routes the traffic: incoming bytes go to the log, to the
// receive-trigger matcher, and to the script stream; sends go out through the
// CommManager and into the log.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    // While the Send Sequence Parameter dialog is open, Docklight pauses
    // communication: no receive processing and no periodical sends.
    Q_PROPERTY(bool dialogPause WRITE setDialogPause NOTIFY stateChanged)
    // Docklight's Snapshot: run communication with the window frozen until a
    // trigger sequence fires, then reveal the buffered data and stop.
    Q_PROPERTY(bool snapshotArmed READ snapshotArmed NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty WRITE setDirty NOTIFY stateChanged)
    Q_PROPERTY(QString projectPath READ projectPath WRITE setProjectPath NOTIFY stateChanged)
    // Docklight's recent-files list (File menu), persisted across sessions.
    Q_PROPERTY(QStringList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    Q_PROPERTY(QString commStateText READ commStateText NOTIFY stateChanged)

    Q_PROPERTY(CommManager *comm READ comm CONSTANT)
    Q_PROPERTY(ChannelSettings *channelA READ channelA CONSTANT)
    Q_PROPERTY(ChannelSettings *channelB READ channelB CONSTANT)
    Q_PROPERTY(SequenceModel *sequences READ sequences CONSTANT)
    Q_PROPERTY(CommLog *log READ log CONSTANT)
    Q_PROPERTY(ScriptRunner *scripts READ scripts CONSTANT)
    // Docklight's continuous communication logging (F2 start, F3 stop).
    Q_PROPERTY(bool fileLogging READ fileLogging NOTIFY stateChanged)
    Q_PROPERTY(PortEnumerator *ports READ ports CONSTANT)
    Q_PROPERTY(PortalPicker *picker READ picker CONSTANT)
    Q_PROPERTY(QString scriptSource READ scriptSource WRITE setScriptSource NOTIFY scriptChanged)
    // Docklight "Customize / External Editor": command template with %FILE%
    // and %LINE% wildcards; empty = use the built-in editor.
    Q_PROPERTY(QString externalEditor READ externalEditor WRITE setExternalEditor NOTIFY externalEditorChanged)
    // Docklight Expert Options: "high priority process" timing mode, adapted
    // to Linux (rtkit -> sched_setscheduler -> nice, best effort).
    Q_PROPERTY(bool highPriorityTiming READ highPriorityTiming
               WRITE setHighPriorityTiming NOTIFY highPriorityTimingChanged)
    // Window geometry (x, y, w, h) persisted with the session.
    Q_PROPERTY(QByteArray windowGeometry READ windowGeometry
               WRITE setWindowGeometry NOTIFY stateChanged)
    // Lucidlight extension: keep retrying a failed serial channel every
    // 2 s while communication runs (USB unplug/replug).
    Q_PROPERTY(bool autoReopen READ autoReopen WRITE setAutoReopen
               NOTIFY autoReopenChanged)
    // Lucidlight extension: annotate valid Modbus RTU frames in the log.
    Q_PROPERTY(bool modbusDecode READ modbusDecode WRITE setModbusDecode
               NOTIFY stateChanged)
    // Docklight's project option: run the script when the project opens.
    Q_PROPERTY(bool runOnOpen READ runOnOpen WRITE setRunOnOpen NOTIFY stateChanged)
    // Docklight's "additional date/time stamp": period in seconds for the
    // clock stamps written into the communication window, 0 = off.
    Q_PROPERTY(int clockStampInterval READ clockStampInterval WRITE setClockStampInterval NOTIFY clockStampChanged)
    // Docklight's Tools > Keyboard Console: typed keys go straight to the
    // port (Send/Receive mode only).
    Q_PROPERTY(bool keyboardConsole READ keyboardConsole WRITE setKeyboardConsole NOTIFY keyboardConsoleChanged)

public:
    explicit Backend(QObject *parent = nullptr);

    bool running() const { return m_comm->running(); }
    bool dirty() const { return m_dirty; }
    void setDirty(bool dirty);
    QString projectPath() const { return m_projectPath; }
    void setProjectPath(const QString &path);
    QStringList recentFiles() const { return m_recentFiles; }
    Q_INVOKABLE void clearRecentFiles();
    // Sequence library: share sequences between projects (.llseq JSON).
    Q_INVOKABLE bool exportSequences(const QString &path);
    Q_INVOKABLE int importSequences(const QString &path); // returns imported count
    void pushRecentFile(const QString &path);
    QString commStateText() const;

    CommManager *comm() const { return m_comm; }
    ChannelSettings *channelA() const { return m_a; }
    ChannelSettings *channelB() const { return m_b; }
    SequenceModel *sequences() const { return m_sequences; }
    CommLog *log() const { return m_log; }
    ScriptRunner *scripts() const { return m_runner; }
    Q_INVOKABLE bool scriptDispatch(const QString &path);
    void stopDispatches();
    int dispatchCount() const { return m_dispatches.size(); }   // test/agent hook
    PortEnumerator *ports() const { return m_ports; }
    PortalPicker *picker() const { return m_picker; }
    QString scriptSource() const { return m_scriptSource; }
    QString externalEditor() const { return m_externalEditor; }
    bool highPriorityTiming() const { return m_highPriorityTiming; }
    void setHighPriorityTiming(bool on);
    bool applyHighPriority();
    QByteArray windowGeometry() const { return m_windowGeometry; }
    void setWindowGeometry(const QByteArray &geometry);
    bool autoReopen() const { return m_autoReopen; }
    void setAutoReopen(bool on);
    Q_INVOKABLE void autoReopenCheck();   // one poll; also the test hook
    bool modbusDecode() const { return m_modbusDecode; }
    void setModbusDecode(bool on);
    void updateReopenTimer();
    void setExternalEditor(const QString &cmd);
    Q_INVOKABLE bool editScriptExternally();
    void setScriptSource(const QString &source);
    bool runOnOpen() const;
    void setRunOnOpen(bool on);
    int clockStampInterval() const { return m_clockStampInterval; }
    void setClockStampInterval(int seconds);
    bool keyboardConsole() const { return m_keyboardConsole; }
    void setKeyboardConsole(bool on);
    void runScriptFile(const QString &path);
    // Runs script text directly (the script panel calls scripts.run).
    Q_INVOKABLE void runScript(const QString &source);

    // ---- actions from QML -------------------------------------------------
    Q_INVOKABLE void toggleComm();
    Q_INVOKABLE bool sendText(const QString &text, int format);
    Q_INVOKABLE void sendSequence(int row);
    Q_INVOKABLE void sendByHotkey(int keyNumber);   // 1..12 for F1..F12
    Q_INVOKABLE void clearLog();
    Q_INVOKABLE void saveLogTo(const QString &path);
    // Renders the whole communication window (all rows, all columns) into a
    // PDF file — the Docklight "Print" for a windowless Linux world.
    Q_INVOKABLE void printLogTo(const QString &path);
    Q_INVOKABLE void saveScriptTo(const QString &path);
    Q_INVOKABLE void newProject();
    Q_INVOKABLE void openProjectFrom(const QString &path);
    Q_INVOKABLE void saveProjectTo(const QString &path);
    Q_INVOKABLE void saveSession();                 // autosave to the state dir
    // Returns true when a saved session was restored; false on first run,
    // so the caller can clear the fake dirty state from the welcome data.
    Q_INVOKABLE bool loadSession();
    Q_INVOKABLE QString parseError(const QString &text, int format) const;
    Q_INVOKABLE QString previewBytes(const QString &text, int format) const;
    Q_INVOKABLE void copyToClipboard(const QString &text);
    Q_INVOKABLE QString clipboardText() const;

    // Extra bytes above the last log entry, for the highlight pass.
    static constexpr int NoChannel = -1;

    // ---- script bridge (worker thread calls these via invokeMethod) --------
    Q_INVOKABLE int scriptReceiveCount(int id);
    Q_INVOKABLE void scriptResetCounters(int id);          // -1 = all
    Q_INVOKABLE QString scriptDrainReceiveComments();
    Q_INVOKABLE QString scriptChannelSettings(int channel);
    Q_INVOKABLE bool scriptSetChannelSettings(const QString &settings, int channel);
    // Single queue-safe entry point for the script engine: dispatches by
    // name with QVariantList arguments, avoiding raw metatype marshalling.
    Q_INVOKABLE QVariant scriptInvoke(const QString &name, const QVariantList &args);
    Q_INVOKABLE int scriptChannelStatus(int channel);
    Q_INVOKABLE bool scriptOpenProject(const QString &path);
    Q_INVOKABLE void scriptSetContentsFilter(int filter);
    Q_INVOKABLE void scriptSetWindowLayout(const QString &layout);
    Q_INVOKABLE QString scriptCommWindowData(const QString &representation);
    // UI entry points for the file logger (F2/F3).
    Q_INVOKABLE void startFileLogging();
    Q_INVOKABLE void stopFileLogging();
    Q_INVOKABLE bool fileLogging() const;
    Q_INVOKABLE bool scriptStartLogging(const QString &path, bool appendData,
                                        const QString &representations, int format,
                                        bool noHeaders);
    Q_INVOKABLE void scriptStopLogging();
    Q_INVOKABLE void scriptSetHtmlStyle(const QString &css) {
        m_logger.setHtmlStyle(css);
    }
    Q_INVOKABLE void scriptSetMicroStamps(bool on) {
        m_log->setMicroStamps(on);
        m_logger.setMicroStamps(on);
    }
    Q_INVOKABLE bool scriptGetMicroStamps() const {
        return m_log->microStamps();
    }
    Q_INVOKABLE QString scriptGetHtmlStyle() const {
        return m_logger.htmlStyle();
    }
    Q_INVOKABLE void scriptSetUserOutput(const QString &text);
    Q_INVOKABLE QString scriptPortList();
    Q_INVOKABLE bool scriptLoadOptions(const QString &path);
    Q_INVOKABLE bool scriptSaveOptions(const QString &path);
    // The heart of DL.SendSequence: builds the parameter/checksum-processed
    // element stream, runs the DL_OnSend event procedure when a script is
    // parked, and transmits with delays, breaks and signal changes.
    Q_INVOKABLE void scriptSendSequence(int row, const QString &customData,
                                        const QString &representation,
                                        const QStringList &parameters);
    // Manual send with wildcard parameters answered by the dialog.
    Q_INVOKABLE void sendSequenceWithParams(int row, const QStringList &parameters);
    // Wildcard parameter areas (minimum lengths) of a sequence, for the
    // Send Sequence Parameter dialog.
    Q_INVOKABLE QStringList sequenceWildcardAreas(int row) const;
    // QML answers a script dialog; value -1 = input box text via textValue.
    Q_INVOKABLE void answerScriptDialog(int value, const QString &textValue);

    // Docklight Snapshot.
    Q_INVOKABLE void toggleSnapshot();
    bool snapshotArmed() const { return m_snapshotArmed; }

public slots:
    void scriptDialog(const QString &prompt, int buttons, const QString &title,
                      bool input, const QString &defaultText);

signals:
    void clockStampChanged();
    void keyboardConsoleChanged();
    void stateChanged();
    void recentFilesChanged();
    void autoReopenChanged();
    void scriptChanged();
    void externalEditorChanged();
    void highPriorityTimingChanged();
    void toast(const QString &text);
    // Blocking script dialogs (DL.MsgBox2 / DL.InputBox2); QML shows the
    // dialog and answers via answerScriptDialog.
    void scriptDialogRequested(const QString &prompt, int buttons, const QString &title,
                               bool input, const QString &defaultText);
    // Sequence with wildcards needs parameter values before it can be sent.
    void parameterRequest(int row, const QStringList &minLengths);
    void userOutputChanged(const QString &text);
    void contentsFilterChanged(int filter);
    void windowLayoutChanged(const QString &layout);

private slots:
    void handleIncoming(int channel, const QByteArray &data);
    void setDialogPause(bool paused);
    void handleTransmitted(int channel, const QByteArray &data);
    void handleTrigger(int sequenceId, const QVector<SequenceMatcher::Token> &tokens, int channel);
    void checkRepeats();
    void scriptFinished(bool ok, const QString &error);

private slots:
    void scriptSendSequenceDone();

private:
    void transmitElements(const QVector<ParsedBytes::Element> &elements);
    bool applyOnSend(const Sequence &seq, QVector<ParsedBytes::Element> &elements, int rowIndex = -1);
    bool buildSendElements(const Sequence &seq, const QStringList &parameters,
                           DataFormat::Format paramFormat,
                           QVector<ParsedBytes::Element> *out, QString *error);
    void wireDirtyTracking();
    void applyTrigger(const Sequence *receiveSequence, const QByteArray &matched, int channel);
    void syncMatcher();
    QString expandCommentMacros(const QString &text, const Sequence *seq,
                                const QByteArray &received, int channel);
    int indexOfSequence(int id) const;
    void armRepeat(const Sequence *seq, const QStringList &parameters = QStringList());
    void logComment(const QString &text);
    void logEvent(const QString &text);

    CommManager *m_comm;
    ChannelSettings *m_a;
    ChannelSettings *m_b;
    SequenceModel *m_sequences;
    CommLog *m_log;
    ScriptRunner *m_runner;
    QVector<ScriptRunner *> m_dispatches;
    PortEnumerator *m_ports;
    PortalPicker *m_picker;
    // One matcher per main port: a pattern must never match across a
    // channel boundary (Docklight matches each port's stream separately).
    SequenceMatcher m_matchers[2];
    FileLogger m_logger;

    bool m_dirty = false;
    QString m_projectPath;
    QString m_scriptSource;
    QString m_externalEditor;
    bool m_highPriorityTiming = false;
    QByteArray m_windowGeometry;
    bool m_autoReopen = false;
    bool m_modbusDecode = false;
    QTimer *m_reopenTimer = nullptr;
    QFileSystemWatcher *m_scriptWatcher = nullptr;
    QString m_scriptTempFile;
    bool m_runOnOpen = false;
    bool m_pendingHighlight = false;

    QHash<int, int> m_receiveCounts;   // Docklight Receive Sequence hit counters
    QStringList m_receiveComments;     // drained by DL.GetReceiveComments

    QTimer m_repeatTimer;
    QTimer m_clockTimer;
    int m_clockStampInterval = 0;
    bool m_dialogPause = false;
    bool m_snapshotArmed = false;
    QStringList m_recentFiles;
    bool m_snapshotTriggered = false;
    qint64 m_snapshotRowsBefore = 0;
    qint64 m_snapshotRowsSeen = 0;
    QVector<QPair<int, QByteArray>> m_heldIncoming;   // seconds, 0 = off
    bool m_keyboardConsole = false;
    QHash<int, qint64> m_nextRepeatAt;
    QHash<int, QStringList> m_repeatParameters;   // parameters captured at arm time
    QElapsedTimer m_repeatClock;
};
