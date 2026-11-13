#pragma once

#include <QFile>
#include <QTextStream>

#include <QElapsedTimer>
#include <QJSValue>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QVector>
#include <atomic>
#include <functional>

#include "sequencemodel.h"

class CommManager;
class QJSEngine;
class Backend;

// The worker-side engine: hosts QJSEngine, owns the DL object, and buffers
// the incoming stream so blocking DL.Receive* calls can search it. One
// instance per run, living on a worker thread.
class DLScriptEngine : public QObject {
    Q_OBJECT

public:
    // A copy-safe snapshot of the sequences, taken on the main thread when
    // the run starts.
    struct Snapshot {
        struct Entry {
            int id = 0;
            QString name;
            bool isSend = true;
            DataFormat::Format format = DataFormat::Format::Text;
            QByteArray bytes;
            QVector<bool> wildcard;
        };
        QVector<Entry> entries;
    };

    explicit DLScriptEngine(QObject *parent = nullptr);
    ~DLScriptEngine() override;

    // Main thread only, before the worker starts.
    void configure(CommManager *comm, const Snapshot &snapshot, QObject *bridge);
    QJSEngine *jsEngine() const { return m_engine; }
    bool isParked() const { return m_parked; }

    // One detected Receive Sequence, queued from the main thread and
    // processed between script statements.
    struct ReceiveEvent {
        int id = 0;
        int index = 0;
        QString name;
        QByteArray bytes;
        int channel = 1;
        qint64 timestampMs = 0;
    };

public slots:
    // Worker thread. Evaluates the script and reports the outcome.
    void run(QString code);
    void onIncoming(int channel, const QByteArray &data);
    void onStop();
    void onCommStateChanged(bool running);
    // Queued from Backend::handleTrigger while a script is running.
    void onReceiveEvent(int id, int index, const QString &name,
                        const QByteArray &bytes, int channel, qint64 timestampMs);
    // Posted back after Backend handled a DL.SendSequence request.
    void onSendRequestDone();
    // Called synchronously from the main thread before a script-generated
    // sequence goes out; runs the DL_OnSend() event procedure if defined.
    QByteArray processOnSend(const QString &name, int index, const QByteArray &bytes);
    // Resolves the pending dialog request; called from the main thread.
    void onDialogResult(int result);
    // Text answer for DL.InputBox2.
    void onDialogText(const QString &text);

signals:
    void incomingWakeup();
    void outputLine(const QString &line, bool error);
    void messageRequested(const QString &text);
    void commentRequested(const QString &text, bool pendingLine = false);
    void clearRequested();
    void saveLogRequested(const QString &path);
    void finished(bool ok, const QString &error);
    // DL.SendSequence: queued to Backend::scriptSendSequence.
    void sendRequest(int row, const QString &customData, const QString &representation,
                     const QStringList &parameters);
    // Blocking dialogs (DL.MsgBox2 / DL.InputBox2).
    void dialogRequest(const QString &prompt, int buttons, const QString &title,
                       bool input, const QString &defaultText);

private:
    friend class DLObject;

    // Parks in a local event loop until the predicate is true, the timeout
    // passes, or a stop is requested. Queued DL_OnReceive events are
    // processed between park slices, mirroring Docklight's event timing.
    bool waitUntil(const std::function<bool()> &predicate, int timeoutMs);
    void processReceiveEvents();
    // Blocking call into the bridge object living on the main thread.
    QVariant callBridge(const char *method, const QVariantList &args = {});

    QJSEngine *m_engine = nullptr;
    CommManager *m_comm = nullptr;
    Snapshot m_snapshot;
    QObject *m_bridge = nullptr;

    QMutex m_streamMutex;
    QByteArray m_stream;

    std::atomic_bool m_stop {false};
    std::atomic_bool m_commRunning {false};
    std::atomic_bool m_parked {false};
    std::atomic_bool m_quit {false};
    bool m_broken = false;
    bool m_failed = false;
    QString m_failMessage;
    qint64 m_startMs = 0;
    QString m_lastError;

    QVector<ReceiveEvent> m_receiveQueue;
    ReceiveEvent m_onReceive;         // context for DL.OnReceive_*
    bool m_onSendActive = false;      // context for DL.OnSend_*
    QString m_onSendName;
    int m_onSendIndex = -1;
    QByteArray m_onSendData;
    // DL.SendSequence handshake with the main thread.
    bool m_sendDone = false;
    // DL.MsgBox2 / DL.InputBox2 handshake.
    bool m_dialogDone = false;
    int m_dialogResult = 0;
    QString m_inputResult;
    bool m_inputReady = false;
};

// The scripting DL object, the Lucidlight counterpart of Docklight's VBScript
// DL API. One instance is created inside the worker thread for every run;
// every method below is called from script code.
//
// Send-family functions accept Lucidlight's project notation; Receive-family
// functions block the script (not the UI) until the data arrives or the
// timeout passes, exactly like their Docklight counterparts.
class DLObject : public QObject {
    Q_OBJECT

public:
    explicit DLObject(DLScriptEngine *host);

    // ---- communication ----------------------------------------------------
    Q_INVOKABLE bool StartCommunication();
    Q_INVOKABLE void StopCommunication();
    Q_INVOKABLE bool IsCommRunning() const;
    Q_INVOKABLE bool SetCommMode(int mode);
    Q_INVOKABLE void ClearCommWindows();
    Q_INVOKABLE void ClearComm() { ClearCommWindows(); }   // Lucidlight alias

    // ---- sending ------------------------------------------------------------
    Q_INVOKABLE bool Send(const QString &data);          // project notation, Text mode
    Q_INVOKABLE bool SendHex(const QString &hex);
    Q_INVOKABLE bool SendStr(const QString &text);       // UTF-8 encoded
    Q_INVOKABLE bool SendBytes(const QJSValue &array);   // JS array of 0..255
    // Docklight: DL.SendSequence nameOrIndex [, parameters] [, representation]
    Q_INVOKABLE bool SendSequence(const QJSValue &nameOrIndex,
                                  const QString &parameters = QString(),
                                  const QString &representation = QStringLiteral("A"));
    Q_INVOKABLE bool DirectSend(int channelNo, const QString &customSequence,
                                const QString &representation = QStringLiteral("A"));
    // Side channels 3..10, Docklight's extra scripting connections.
    Q_INVOKABLE bool OpenSideChannel(int channelNo, const QString &device);
    Q_INVOKABLE void CloseSideChannel(int channelNo);
    Q_INVOKABLE void SendBreak(int durationMs = 200);
    // Docklight: DL.SetHandshakeSignals rts, dtr — plus bit-coded reader.
    Q_INVOKABLE void SetHandshakeSignals(bool rts, bool dtr);
    Q_INVOKABLE void SetDTR(bool on) { SetHandshakeSignals(false, on); }
    Q_INVOKABLE void SetRTS(bool on) { SetHandshakeSignals(on, false); }
    Q_INVOKABLE int GetHandshakeSignals();
    Q_INVOKABLE bool GetCTS();
    Q_INVOKABLE bool GetDSR();
    Q_INVOKABLE bool GetDCD();
    Q_INVOKABLE bool GetRI();

    // ---- receiving (blocks the script, never the UI) ------------------------
    // DL.Receive(count, timeoutMs) → string of exactly that many bytes, or false.
    Q_INVOKABLE QJSValue Receive(int count, int timeoutMs);
    // DL.ReceiveStr(text, timeoutMs) → true when the text shows up in the stream.
    Q_INVOKABLE bool ReceiveStr(const QString &text, int timeoutMs);
    // DL.ReceiveHex(pattern, timeoutMs) → hex of the matched bytes, or false.
    // ?? inside the pattern matches one unknown byte each.
    Q_INVOKABLE QJSValue ReceiveHex(const QString &pattern, int timeoutMs);
    // Docklight: result = DL.WaitForSequence(nameOrIndex [, maxCounter]
    // [, timeout]) → the hit counter value after waiting.
    Q_INVOKABLE int WaitForSequence(const QJSValue &nameOrIndex, int maxCounter = 1,
                                    int timeout = -1);
    // Lucidlight extension: the next line without its CR/LF, or false.
    Q_INVOKABLE QJSValue ReceiveLine(int timeoutMs);
    // Docklight counters.
    Q_INVOKABLE int GetReceiveCounter(const QJSValue &nameOrIndex);
    Q_INVOKABLE void ResetReceiveCounter(const QJSValue &nameOrIndex = QJSValue());
    Q_INVOKABLE QString GetReceiveComments();
    Q_INVOKABLE int NoOfSendSequences() const;
    Q_INVOKABLE int NoOfReceiveSequences() const;

    // ---- test verdicts for automated runs -----------------------------------
    // DL.Fail(message) stops the script and fails the run (exit code 1).
    Q_INVOKABLE void Fail(const QString &message);
    // DL.Assert(condition, message) fails the run when the condition is false.
    Q_INVOKABLE void Assert(const QJSValue &condition, const QString &message);
    // DL.Env(name, fallback) reads an environment variable, so test runners
    // can pass device paths and expectations without editing the script.
    Q_INVOKABLE QString Env(const QString &name, const QString &fallback = QString());
    // DL.SaveLog(path) writes the communication window so far to a file.
    Q_INVOKABLE void SaveLog(const QString &path);

    // ---- timing and control ---------------------------------------------------
    Q_INVOKABLE void Wait(int ms);
    Q_INVOKABLE void Pause(int ms = 0);
    Q_INVOKABLE void Break();
    Q_INVOKABLE int StartTime();
    Q_INVOKABLE double TimeDiff();

    // ---- communication window -------------------------------------------------
    // Docklight: DL.AddComment [comment] [, timeStampAfterComment]
    // [, lineBreakAndPadding]
    Q_INVOKABLE void AddComment(const QString &comment = QString(),
                                bool timeStampAfterComment = false,
                                bool lineBreakAndPadding = true);
    Q_INVOKABLE void Comment(const QString &text) { AddComment(text); }
    Q_INVOKABLE QString GetCommWindowData(const QString &representation = QStringLiteral("A"));
    Q_INVOKABLE void SetContentsFilter(int filter);
    Q_INVOKABLE QString GetDocklightTimeStamp(const QJSValue &dateTime = QJSValue(),
                                              int milliseconds = -1, bool trimmed = false);

    // ---- conversions, matching the classic string helpers ----------------------
    Q_INVOKABLE QString HexToStr(const QString &hex);   // byte → char, 1:1 (Latin-1)
    Q_INVOKABLE QString StrToHex(const QString &text);
    Q_INVOKABLE int HexToDec(const QString &hex);
    Q_INVOKABLE QString DecToHex(int value, int digits = 2);
    Q_INVOKABLE int Asc(const QString &text);
    Q_INVOKABLE QString Chr(int code);

    // ---- advanced: checksums and conversions ---------------------------------
    Q_INVOKABLE QString CalcChecksum(const QString &checksumSpec, const QString &dataStr,
                                     const QString &representation = QStringLiteral("H"),
                                     int startPos = 1, int endPos = 0,
                                     bool bigEndian = true);
    Q_INVOKABLE QString ConvertSequenceData(const QString &conversionType,
                                            const QString &source,
                                            const QString &representation = QStringLiteral("H"),
                                            bool bigEndian = true);

    // ---- advanced: channels, project, files, dialogs ---------------------------
    Q_INVOKABLE QString GetChannelSettings(int channelNo = 1);
    Q_INVOKABLE bool SetChannelSettings(const QString &newSettings, int channelNo = 1,
                                        bool dontTest = false);
    Q_INVOKABLE int GetChannelStatus(int channelNo = 1);
    Q_INVOKABLE QString GetEnvironment(const QString &name);
    Q_INVOKABLE QString GetEnvironmentVariable(const QString &name) { return GetEnvironment(name); }
    Q_INVOKABLE int GetKeyState(int virtKey);
    Q_INVOKABLE void OpenProject(const QString &filePathName);
    Q_INVOKABLE void LoadProgramOptions(const QString &filePathName);
    Q_INVOKABLE void SaveProgramOptions(const QString &filePathName);
    Q_INVOKABLE bool StartLogging(const QString &baseFilePath, bool appendData = true,
                                  const QString &representations = QStringLiteral("AHDB"),
                                  int format = 0, bool highspeed = false,
                                  bool noHeaders = false);
    Q_INVOKABLE void StopLogging();
    Q_INVOKABLE void PlaybackLogFile(const QString &filePathName,
                                     const QString &dataDirection = QString(),
                                     int timeInterval = 0);
    Q_INVOKABLE void UploadFile(const QString &filePathName,
                                const QString &representation = QStringLiteral("A"));
    Q_INVOKABLE int ShellRun(const QString &operation, const QString &file,
                             const QString &parameters = QString(),
                             const QString &directory = QString(), int showCmd = 1);
    Q_INVOKABLE int MsgBox2(const QString &prompt, int buttons = 0,
                            const QString &title = QString());
    Q_INVOKABLE QString InputBox2(const QString &prompt, const QString &title = QString(),
                                  const QString &defaultText = QString());
    Q_INVOKABLE void SetUserOutput(const QString &text, bool rtfFormat = false,
                                   bool append = true, bool readFromFile = false);
    // Docklight's name for the same user output tab.
    Q_INVOKABLE void SetWindowOutput(const QString &text, bool rtfFormat = false,
                                     bool append = true, bool readFromFile = false) {
        SetUserOutput(text, rtfFormat, append, readFromFile);
    }
    Q_INVOKABLE void SetWindowLayout(const QString &layout = QStringLiteral("SD"));
    Q_INVOKABLE void Quit();

    // ---- DL_OnSend() / DL_OnReceive() context ---------------------------------
    Q_INVOKABLE int OnSend_GetSize() const;
    Q_INVOKABLE QString OnSend_GetName() const;
    Q_INVOKABLE int OnSend_GetIndex() const;
    Q_INVOKABLE QString OnSend_GetData(const QString &representation = QStringLiteral("A"),
                                       int start = 1, int length = -1);
    Q_INVOKABLE void OnSend_SetData(const QString &newData,
                                    const QString &representation = QStringLiteral("A"));
    Q_INVOKABLE QJSValue OnSend_Peek(int charNo, const QString &representation = QString());
    Q_INVOKABLE void OnSend_Poke(int charNo, int value);
    Q_INVOKABLE int OnReceive_GetSize() const;
    Q_INVOKABLE QString OnReceive_GetName() const;
    Q_INVOKABLE int OnReceive_GetIndex() const;
    Q_INVOKABLE QString OnReceive_GetData(const QString &representation = QStringLiteral("A"),
                                          int start = 1, int length = -1);
    Q_INVOKABLE QJSValue OnReceive_Peek(int charNo, const QString &representation = QString());
    Q_INVOKABLE int OnReceive_GetChannel() const;
    Q_INVOKABLE double OnReceive_GetDateTime() const;
    Q_INVOKABLE int OnReceive_GetMilliseconds() const;

    // ---- file access (FileInput / FileOutput helpers) --------------------------
    Q_INVOKABLE QJSValue _openInput(const QString &path);
    Q_INVOKABLE QJSValue _openOutput(const QString &path, bool append);

    // ---- misc -------------------------------------------------------------------
    Q_INVOKABLE void ShowMessage(const QString &text);
    Q_INVOKABLE QString GetLastError() const;
    Q_INVOKABLE void _print(const QString &text);

private:
    friend class DLScriptEngine;
    bool sendBytes(const QByteArray &bytes);
    QByteArray takeStream(int end, bool *ok);
    int resolveSequenceId(const QJSValue &nameOrIndex, bool send) const;

    DLScriptEngine *m_host;
    int m_matchEnd = 0;
};

// Runs scripts on a worker thread. The UI talks to this object only.
class ScriptRunner : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    // bridge: a main-thread object with Q_INVOKABLE script* helpers the DL
    // API calls into (counters, logging, project handling).
    ScriptRunner(CommManager *comm, SequenceModel *sequences, QObject *bridge,
                 QObject *parent = nullptr);
    ~ScriptRunner() override;

    bool running() const { return m_running; }
    DLScriptEngine *engine() const { return m_engine; }
    Q_INVOKABLE void run(const QString &code);
    // Runs a script from a file; the file's directory is the base for
    // #include directives, like Docklight's script preprocessor.
    Q_INVOKABLE void runFile(const QString &path);
    Q_INVOKABLE void stop();

signals:
    void runningChanged();
    void outputLine(const QString &line, bool error);
    void messageRequested(const QString &text);
    void commentRequested(const QString &text, bool pendingLine = false);
    void clearRequested();
    void saveLogRequested(const QString &path);
    void finished(bool ok, const QString &error);
    void sendRequest(int row, const QString &customData, const QString &representation,
                     const QStringList &parameters);
    void dialogRequest(const QString &prompt, int buttons, const QString &title,
                       bool input, const QString &defaultText);

private slots:
    void handleFinished(bool ok, const QString &error);

private:
    DLScriptEngine::Snapshot snapshot() const;

    CommManager *m_comm;
    SequenceModel *m_sequences;
    QObject *m_bridge = nullptr;
    QThread m_thread;
    DLScriptEngine *m_engine = nullptr;
    QString m_code;
    bool m_running = false;
};

// Sequential file reader, the Lucidlight counterpart of the Docklight
// FileInput object.
class FileInputObject : public QObject {
    Q_OBJECT
public:
    explicit FileInputObject(const QString &path);
    Q_INVOKABLE bool isOpen() const { return m_open; }
    Q_INVOKABLE bool atEnd() const { return !m_open || m_file.atEnd(); }
    Q_INVOKABLE QString readChar();
    Q_INVOKABLE QString readLine();
    Q_INVOKABLE QString readAll();
    Q_INVOKABLE void close();

private:
    QFile m_file;
    QTextStream m_stream;
    bool m_open = false;
};

// File writer, the counterpart of the Docklight FileOutput object.
class FileOutputObject : public QObject {
    Q_OBJECT
public:
    FileOutputObject(const QString &path, bool append);
    Q_INVOKABLE bool isOpen() const { return m_open; }
    Q_INVOKABLE void write(const QString &text);
    Q_INVOKABLE void writeLine(const QString &text);
    Q_INVOKABLE void writeHex(const QString &hex);
    Q_INVOKABLE void close();

private:
    QFile m_file;
    QTextStream m_stream;
    bool m_open = false;
};

