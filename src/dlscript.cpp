#include "dlscript.h"

#include <QEventLoop>
#include <QJSEngine>
#include <QTimer>

#include "commmanager.h"

namespace {
// Blocking DL calls park the worker in this loop, so queued cross-thread
// deliveries still arrive while a script waits. Slices stay short (50 ms) so
// a Stop request is noticed quickly even without incoming data.
// Returns true when the wake-up came from the data signal.
bool park(QEventLoop &loop, QObject *wakeupSource, int remainingMs) {
    QTimer kicker;
    kicker.setSingleShot(true);
    kicker.start(remainingMs < 0 ? 50 : qMin(50, remainingMs));
    QMetaObject::Connection byData;
    if (wakeupSource)
        byData = QObject::connect(wakeupSource, SIGNAL(incomingWakeup()), &loop, SLOT(quit()));
    const QMetaObject::Connection byTime =
        QObject::connect(&kicker, &QTimer::timeout, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(byData);
    QObject::disconnect(byTime);
    return !kicker.isActive();
}
}  // namespace

DLScriptEngine::DLScriptEngine(QObject *parent) : QObject(parent) {}

DLScriptEngine::~DLScriptEngine() = default;

void DLScriptEngine::configure(CommManager *comm, const Snapshot &snapshot) {
    m_comm = comm;
    m_snapshot = snapshot;
}

void DLScriptEngine::onIncoming(int channel, const QByteArray &data) {
    Q_UNUSED(channel)
    QMutexLocker lock(&m_streamMutex);
    m_stream.append(data);
    if (m_stream.size() > 65536)
        m_stream.remove(0, m_stream.size() - 65536);
    lock.unlock();
    emit incomingWakeup();
}

void DLScriptEngine::onCommStateChanged(bool running) {
    m_commRunning = running;
}

void DLScriptEngine::onStop() {
    m_stop = true;
}

bool DLScriptEngine::waitUntil(const std::function<bool()> &predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (m_stop)
            return false;
        const int remaining = timeoutMs < 0 ? -1 : timeoutMs - int(timer.elapsed());
        if (remaining <= 0)
            return false;
        QEventLoop loop;
        park(loop, this, remaining);
    }
    return true;
}

void DLScriptEngine::run(const QString &code) {
    m_stop = false;
    m_broken = false;
    m_startMs = QDateTime::currentMSecsSinceEpoch();
    m_lastError.clear();
    {
        QMutexLocker lock(&m_streamMutex);
        m_stream.clear();
    }

    m_engine = new QJSEngine();
    DLObject *dl = new DLObject(this);
    QJSValue dlValue = m_engine->newQObject(dl);
    m_engine->globalObject().setProperty("DL", dlValue);

    // print(...) lands in the script output pane, like WScript.Echo.
    m_engine->evaluate(QStringLiteral(
        "function print() { var s = '';"
        "  for (var i = 0; i < arguments.length; ++i) { if (i) s += ' '; s += String(arguments[i]); }"
        "  DL._print(s); }"));

    const QJSValue result = m_engine->evaluate(code, QStringLiteral("script.js"));
    QString error;
    bool ok = true;
    if (m_engine->isInterrupted()) {
        ok = false;
        m_engine->setInterrupted(false);
        error = m_broken ? QStringLiteral("Script stopped by DL.Break") : QStringLiteral("Script stopped");
    } else if (result.isError()) {
        ok = false;
        error = QStringLiteral("%1 at line %2")
                    .arg(result.property("message").toString(),
                         result.property("lineNumber").toString());
    }

    delete m_engine;
    m_engine = nullptr;
    emit finished(ok, error);
}

// ---------------------------------------------------------------------------
// DLObject
// ---------------------------------------------------------------------------

DLObject::DLObject(DLScriptEngine *host) : QObject(host), m_host(host) {}

bool DLObject::StartCommunication() {
    if (!m_host->m_comm)
        return false;
    bool started = false;
    // Blocking call into the main thread, which owns the ports.
    QMetaObject::invokeMethod(m_host->m_comm, "restart", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, started));
    return started;
}

void DLObject::StopCommunication() {
    if (!m_host->m_comm)
        return;
    QMetaObject::invokeMethod(m_host->m_comm, "stop", Qt::QueuedConnection);
}

bool DLObject::IsCommRunning() const {
    return m_host->m_commRunning;
}

bool DLObject::SetCommMode(int mode) {
    if (!m_host->m_comm || m_host->m_commRunning)
        return false;
    QMetaObject::invokeMethod(m_host->m_comm, "setMode", Qt::BlockingQueuedConnection, Q_ARG(int, mode));
    return true;
}

void DLObject::ClearComm() {
    emit m_host->clearRequested();
}

bool DLObject::sendBytes(const QByteArray &bytes) {
    if (!m_host->m_comm || bytes.isEmpty())
        return false;
    bool sent = false;
    QMetaObject::invokeMethod(m_host->m_comm, "send", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, sent), Q_ARG(QByteArray, bytes));
    return sent;
}

bool DLObject::Send(const QString &data) {
    const auto parsed = DataFormat::parse(data, DataFormat::Format::Text);
    if (!parsed.ok()) {
        m_host->m_lastError = parsed.error;
        return false;
    }
    return sendBytes(parsed.bytes);
}

bool DLObject::SendHex(const QString &hex) {
    const auto parsed = DataFormat::parse(hex, DataFormat::Format::Hex);
    if (!parsed.ok()) {
        m_host->m_lastError = parsed.error;
        return false;
    }
    return sendBytes(parsed.bytes);
}

bool DLObject::SendStr(const QString &text) {
    return sendBytes(text.toUtf8());
}

bool DLObject::SendBytes(const QJSValue &array) {
    if (!array.isArray())
        return false;
    const int length = array.property("length").toInt();
    QByteArray bytes;
    for (int i = 0; i < length; ++i) {
        const int value = array.property(quint32(i)).toInt();
        bytes.append(char(qBound(0, value, 255)));
    }
    return sendBytes(bytes);
}

bool DLObject::SendSequence(const QJSValue &nameOrIndex) {
    const auto &entries = m_host->m_snapshot.entries;
    const DLScriptEngine::Snapshot::Entry *found = nullptr;
    if (nameOrIndex.isNumber()) {
        const int row = nameOrIndex.toInt();
        if (row >= 0 && row < entries.size() && entries.at(row).isSend)
            found = &entries.at(row);
    } else {
        const QString name = nameOrIndex.toString();
        for (const auto &entry : entries)
            if (entry.isSend && entry.name.compare(name, Qt::CaseInsensitive) == 0) {
                found = &entry;
                break;
            }
    }
    if (!found) {
        m_host->m_lastError = QStringLiteral("Send sequence not found: %1").arg(nameOrIndex.toString());
        return false;
    }
    return sendBytes(found->bytes);
}

void DLObject::SendBreak(int durationMs) {
    if (!m_host->m_comm)
        return;
    QMetaObject::invokeMethod(m_host->m_comm, "sendBreak", Qt::QueuedConnection, Q_ARG(int, durationMs));
}

void DLObject::SetDTR(bool on) {
    if (m_host->m_comm)
        QMetaObject::invokeMethod(m_host->m_comm, "setDTR", Qt::QueuedConnection, Q_ARG(int, 0),
                                  Q_ARG(bool, on));
}

void DLObject::SetRTS(bool on) {
    if (m_host->m_comm)
        QMetaObject::invokeMethod(m_host->m_comm, "setRTS", Qt::QueuedConnection, Q_ARG(int, 0),
                                  Q_ARG(bool, on));
}

bool DLObject::GetCTS() {
    return m_host->m_comm ? m_host->m_comm->link(0)->cts() : false;
}
bool DLObject::GetDSR() {
    return m_host->m_comm ? m_host->m_comm->link(0)->dsr() : false;
}
bool DLObject::GetDCD() {
    return m_host->m_comm ? m_host->m_comm->link(0)->dcd() : false;
}
bool DLObject::GetRI() {
    return m_host->m_comm ? m_host->m_comm->link(0)->ri() : false;
}

QByteArray DLObject::takeStream(int end, bool *ok) {
    QMutexLocker lock(&m_host->m_streamMutex);
    *ok = true;
    const QByteArray consumed = m_host->m_stream.left(end);
    m_host->m_stream.remove(0, end);
    return consumed;
}

QJSValue DLObject::Receive(int count, int timeoutMs) {
    QByteArray consumed;
    const bool arrived = m_host->waitUntil(
        [this, count]() {
            QMutexLocker lock(&m_host->m_streamMutex);
            return m_host->m_stream.size() >= count;
        },
        timeoutMs);
    if (!arrived || m_host->m_stop) {
        m_host->m_lastError = QStringLiteral("Receive timed out");
        return QJSValue(false);
    }
    bool ok = false;
    consumed = takeStream(count, &ok);
    if (!ok)
        return QJSValue(false);
    return m_host->jsEngine()->toScriptValue(QString::fromLatin1(consumed));
}

bool DLObject::ReceiveStr(const QString &text, int timeoutMs) {
    if (text.isEmpty())
        return false;
    const QByteArray needle = text.toUtf8();
    const bool arrived = m_host->waitUntil(
        [this, needle]() {
            QMutexLocker lock(&m_host->m_streamMutex);
            return m_host->m_stream.indexOf(needle) >= 0;
        },
        timeoutMs);
    if (!arrived || m_host->m_stop) {
        m_host->m_lastError = QStringLiteral("Receive timed out waiting for %1").arg(text);
        return false;
    }
    bool ok = false;
    QMutexLocker lock(&m_host->m_streamMutex);
    const int at = m_host->m_stream.indexOf(needle);
    if (at < 0)
        return false;
    m_host->m_stream.remove(0, at + needle.size());
    ok = true;
    lock.unlock();
    return ok;
}

QJSValue DLObject::ReceiveHex(const QString &pattern, int timeoutMs) {
    const auto parsed = DataFormat::parse(pattern, DataFormat::Format::Hex);
    if (!parsed.ok()) {
        m_host->m_lastError = parsed.error;
        return QJSValue(false);
    }
    const QByteArray wanted = parsed.bytes;
    const QVector<bool> wildcard = parsed.wildcard;
    auto matchesAt = [&](const QByteArray &buffer, int start) {
        for (int i = 0; i < wanted.size(); ++i)
            if (!wildcard.at(i) && buffer.at(start + i) != wanted.at(i))
                return false;
        return true;
    };
    const bool arrived = m_host->waitUntil(
        [&, this]() {
            QMutexLocker lock(&m_host->m_streamMutex);
            if (m_host->m_stream.size() < wanted.size())
                return false;
            for (int start = 0; start + wanted.size() <= m_host->m_stream.size(); ++start)
                if (matchesAt(m_host->m_stream, start)) {
                    m_matchEnd = start + wanted.size();
                    return true;
                }
            return false;
        },
        timeoutMs);
    if (!arrived || m_host->m_stop) {
        m_host->m_lastError = QStringLiteral("Receive timed out waiting for %1").arg(pattern);
        return QJSValue(false);
    }
    bool ok = false;
    const QByteArray consumed = takeStream(m_matchEnd, &ok);
    if (!ok)
        return QJSValue(false);
    return m_host->jsEngine()->toScriptValue(DataFormat::toHex(consumed));
}

void DLObject::Wait(int ms) {
    m_host->waitUntil([] { return false; }, qMax(0, ms));
}

void DLObject::Pause() {
    QEventLoop loop;
    QTimer::singleShot(0, &loop, &QEventLoop::quit);
    loop.exec();
}

void DLObject::Break() {
    m_host->m_broken = true;
    m_host->m_stop = true;
    if (m_host->jsEngine())
        m_host->jsEngine()->setInterrupted(true);
}

int DLObject::StartTime() {
    return int(QDateTime::currentMSecsSinceEpoch() - m_host->m_startMs);
}

double DLObject::TimeDiff() {
    return double(QDateTime::currentMSecsSinceEpoch() - m_host->m_startMs) / 1000.0;
}

void DLObject::Comment(const QString &text) {
    emit m_host->commentRequested(text);
}

QString DLObject::HexToStr(const QString &hex) {
    const QByteArray bytes = DataFormat::fromHex(hex);
    return QString::fromLatin1(bytes);
}

QString DLObject::StrToHex(const QString &text) {
    return DataFormat::toHex(text.toLatin1());
}

int DLObject::HexToDec(const QString &hex) {
    bool ok = false;
    const int value = hex.toInt(&ok, 16);
    return ok ? value : -1;
}

QString DLObject::DecToHex(int value, int digits) {
    return QString::number(qBound(0, value, 0xFFFFFF), 16).rightJustified(qBound(2, digits, 8), '0').toUpper();
}

int DLObject::Asc(const QString &text) {
    if (text.isEmpty())
        return 0;
    return text.at(0).unicode() & 0xFF;
}

QString DLObject::Chr(int code) {
    return QString(QChar(uchar(qBound(0, code, 255))));
}

void DLObject::ShowMessage(const QString &text) {
    emit m_host->messageRequested(text);
}

QString DLObject::GetLastError() const {
    return m_host->m_lastError;
}

void DLObject::_print(const QString &text) {
    emit m_host->outputLine(text, false);
}

// ---------------------------------------------------------------------------
// ScriptRunner — the main-thread face of the scripting system
// ---------------------------------------------------------------------------

ScriptRunner::ScriptRunner(CommManager *comm, SequenceModel *sequences, QObject *parent)
    : QObject(parent), m_comm(comm), m_sequences(sequences) {}

ScriptRunner::~ScriptRunner() {
    stop();
    if (m_thread.isRunning()) {
        m_thread.quit();
        m_thread.wait(2000);
    }
}

DLScriptEngine::Snapshot ScriptRunner::snapshot() const {
    DLScriptEngine::Snapshot snap;
    for (const Sequence &seq : m_sequences->sequences()) {
        DLScriptEngine::Snapshot::Entry entry;
        entry.id = seq.id;
        entry.name = seq.name;
        entry.isSend = seq.isSend;
        entry.format = seq.format;
        const auto parsed = DataFormat::parse(seq.data, seq.format);
        entry.bytes = parsed.ok() ? parsed.bytes : QByteArray();
        snap.entries.append(entry);
    }
    return snap;
}

void ScriptRunner::run(const QString &code) {
    if (m_running || code.trimmed().isEmpty())
        return;
    m_running = true;
    emit runningChanged();

    m_engine = new DLScriptEngine();
    m_engine->configure(m_comm, snapshot());
    m_engine->onCommStateChanged(m_comm->running());

    // Cross-thread wiring: incoming data and state reach the worker through
    // queued connections; the worker's output flows back the same way.
    connect(m_comm, &CommManager::incoming, m_engine, &DLScriptEngine::onIncoming);
    DLScriptEngine *engine = m_engine;
    connect(m_comm, &CommManager::runningChanged, m_engine,
            [engine, this] { engine->onCommStateChanged(m_comm->running()); });
    connect(&m_thread, &QThread::started, m_engine, [this] { m_engine->run(m_code); });
    connect(m_engine, &DLScriptEngine::outputLine, this, &ScriptRunner::outputLine);
    connect(m_engine, &DLScriptEngine::messageRequested, this, &ScriptRunner::messageRequested);
    connect(m_engine, &DLScriptEngine::commentRequested, this, &ScriptRunner::commentRequested);
    connect(m_engine, &DLScriptEngine::clearRequested, this, &ScriptRunner::clearRequested);
    connect(m_engine, &DLScriptEngine::finished, this, &ScriptRunner::handleFinished);

    m_code = code;
    m_engine->moveToThread(&m_thread);
    m_thread.start();
}

void ScriptRunner::stop() {
    if (!m_running || !m_engine)
        return;
    // Direct cross-thread call: a tight script loop never processes queued
    // events, but QJSEngine interruption is an atomic flag checked by the VM.
    m_engine->onStop();
    if (m_engine->jsEngine())
        m_engine->jsEngine()->setInterrupted(true);
}

void ScriptRunner::handleFinished(bool ok, const QString &error) {
    m_running = false;
    emit runningChanged();
    emit finished(ok, error);
    m_engine->deleteLater();
    m_engine = nullptr;
    m_thread.quit();
    m_thread.wait(2000);
}
