#pragma once

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
        };
        QVector<Entry> entries;
    };

    explicit DLScriptEngine(QObject *parent = nullptr);
    ~DLScriptEngine() override;

    // Main thread only, before the worker starts.
    void configure(CommManager *comm, const Snapshot &snapshot);
    QJSEngine *jsEngine() const { return m_engine; }

public slots:
    // Worker thread. Evaluates the script and reports the outcome.
    void run(const QString &code);
    void onIncoming(int channel, const QByteArray &data);
    void onStop();
    void onCommStateChanged(bool running);

signals:
    void incomingWakeup();
    void outputLine(const QString &line, bool error);
    void messageRequested(const QString &text);
    void commentRequested(const QString &text);
    void clearRequested();
    void finished(bool ok, const QString &error);

private:
    friend class DLObject;

    // Parks in a local event loop until the predicate is true, the timeout
    // passes, or a stop is requested.
    bool waitUntil(const std::function<bool()> &predicate, int timeoutMs);

    QJSEngine *m_engine = nullptr;
    CommManager *m_comm = nullptr;
    Snapshot m_snapshot;

    QMutex m_streamMutex;
    QByteArray m_stream;

    std::atomic_bool m_stop {false};
    std::atomic_bool m_commRunning {false};
    bool m_broken = false;
    qint64 m_startMs = 0;
    QString m_lastError;
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
    Q_INVOKABLE void ClearComm();

    // ---- sending ------------------------------------------------------------
    Q_INVOKABLE bool Send(const QString &data);          // project notation, Text mode
    Q_INVOKABLE bool SendHex(const QString &hex);
    Q_INVOKABLE bool SendStr(const QString &text);       // UTF-8 encoded
    Q_INVOKABLE bool SendBytes(const QJSValue &array);   // JS array of 0..255
    Q_INVOKABLE bool SendSequence(const QJSValue &nameOrIndex);
    Q_INVOKABLE void SendBreak(int durationMs = 200);
    Q_INVOKABLE void SetDTR(bool on);
    Q_INVOKABLE void SetRTS(bool on);
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

    // ---- timing and control ---------------------------------------------------
    Q_INVOKABLE void Wait(int ms);
    Q_INVOKABLE void Pause();
    Q_INVOKABLE void Break();
    Q_INVOKABLE int StartTime();
    Q_INVOKABLE double TimeDiff();

    // ---- communication window -------------------------------------------------
    Q_INVOKABLE void Comment(const QString &text);

    // ---- conversions, matching the classic string helpers ----------------------
    Q_INVOKABLE QString HexToStr(const QString &hex);   // byte → char, 1:1 (Latin-1)
    Q_INVOKABLE QString StrToHex(const QString &text);
    Q_INVOKABLE int HexToDec(const QString &hex);
    Q_INVOKABLE QString DecToHex(int value, int digits = 2);
    Q_INVOKABLE int Asc(const QString &text);
    Q_INVOKABLE QString Chr(int code);

    // ---- misc -------------------------------------------------------------------
    Q_INVOKABLE void ShowMessage(const QString &text);
    Q_INVOKABLE QString GetLastError() const;
    Q_INVOKABLE void _print(const QString &text);

private:
    bool sendBytes(const QByteArray &bytes);
    QByteArray takeStream(int end, bool *ok);

    DLScriptEngine *m_host;
    int m_matchEnd = 0;
};

// Runs scripts on a worker thread. The UI talks to this object only.
class ScriptRunner : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    ScriptRunner(CommManager *comm, SequenceModel *sequences, QObject *parent = nullptr);
    ~ScriptRunner() override;

    bool running() const { return m_running; }
    Q_INVOKABLE void run(const QString &code);
    Q_INVOKABLE void stop();

signals:
    void runningChanged();
    void outputLine(const QString &line, bool error);
    void messageRequested(const QString &text);
    void commentRequested(const QString &text);
    void clearRequested();
    void finished(bool ok, const QString &error);

private slots:
    void handleFinished(bool ok, const QString &error);

private:
    DLScriptEngine::Snapshot snapshot() const;

    CommManager *m_comm;
    SequenceModel *m_sequences;
    QThread m_thread;
    DLScriptEngine *m_engine = nullptr;
    QString m_code;
    bool m_running = false;
};
