#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "channelsettings.h"
#include "commlog.h"
#include "commmanager.h"
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
    Q_PROPERTY(bool dirty READ dirty WRITE setDirty NOTIFY stateChanged)
    Q_PROPERTY(QString projectPath READ projectPath WRITE setProjectPath NOTIFY stateChanged)
    Q_PROPERTY(QString commStateText READ commStateText NOTIFY stateChanged)

    Q_PROPERTY(CommManager *comm READ comm CONSTANT)
    Q_PROPERTY(ChannelSettings *channelA READ channelA CONSTANT)
    Q_PROPERTY(ChannelSettings *channelB READ channelB CONSTANT)
    Q_PROPERTY(SequenceModel *sequences READ sequences CONSTANT)
    Q_PROPERTY(CommLog *log READ log CONSTANT)
    Q_PROPERTY(ScriptRunner *scripts READ scripts CONSTANT)
    Q_PROPERTY(PortEnumerator *ports READ ports CONSTANT)
    Q_PROPERTY(PortalPicker *picker READ picker CONSTANT)
    Q_PROPERTY(QString scriptSource READ scriptSource WRITE setScriptSource NOTIFY scriptChanged)

public:
    explicit Backend(QObject *parent = nullptr);

    bool running() const { return m_comm->running(); }
    bool dirty() const { return m_dirty; }
    void setDirty(bool dirty);
    QString projectPath() const { return m_projectPath; }
    void setProjectPath(const QString &path);
    QString commStateText() const;

    CommManager *comm() const { return m_comm; }
    ChannelSettings *channelA() const { return m_a; }
    ChannelSettings *channelB() const { return m_b; }
    SequenceModel *sequences() const { return m_sequences; }
    CommLog *log() const { return m_log; }
    ScriptRunner *scripts() const { return m_runner; }
    PortEnumerator *ports() const { return m_ports; }
    PortalPicker *picker() const { return m_picker; }
    QString scriptSource() const { return m_scriptSource; }
    void setScriptSource(const QString &source);

    // ---- actions from QML -------------------------------------------------
    Q_INVOKABLE void toggleComm();
    Q_INVOKABLE bool sendText(const QString &text, int format);
    Q_INVOKABLE void sendSequence(int row);
    Q_INVOKABLE void sendByHotkey(int keyNumber);   // 1..12 for F1..F12
    Q_INVOKABLE void clearLog();
    Q_INVOKABLE void saveLogTo(const QString &path);
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

    // Extra bytes above the last log entry, for the highlight pass.
    static constexpr int NoChannel = -1;

signals:
    void stateChanged();
    void scriptChanged();
    void toast(const QString &text);

private slots:
    void handleIncoming(int channel, const QByteArray &data);
    void handleTransmitted(int channel, const QByteArray &data);
    void handleTrigger(int sequenceId, const QByteArray &matched);
    void checkRepeats();
    void scriptFinished(bool ok, const QString &error);

private:
    void wireDirtyTracking();
    void applyTrigger(const Sequence *receiveSequence, const QByteArray &matched);
    void syncMatcher();

    CommManager *m_comm;
    ChannelSettings *m_a;
    ChannelSettings *m_b;
    SequenceModel *m_sequences;
    CommLog *m_log;
    ScriptRunner *m_runner;
    PortEnumerator *m_ports;
    PortalPicker *m_picker;
    SequenceMatcher m_matcher;

    bool m_dirty = false;
    QString m_projectPath;
    QString m_scriptSource;
    bool m_pendingHighlight = false;

    QTimer m_repeatTimer;
    QHash<int, qint64> m_nextRepeatAt;
    QElapsedTimer m_repeatClock;
};
