#include "dlscript.h"

#include <QBuffer>
#include <QMetaMethod>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <cmath>
#include <cstdio>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QProcess>
#include <QTimer>

#include "backend.h"
#include "checksum.h"
#include "commmanager.h"
#include "vbstrans.h"

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

void DLScriptEngine::configure(CommManager *comm, const Snapshot &snapshot, QObject *bridge) {
    m_comm = comm;
    m_snapshot = snapshot;
    m_bridge = bridge;
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

void DLScriptEngine::onReceiveEvent(int id, int index, const QString &name,
                                    const QByteArray &bytes, int channel,
                                    qint64 timestampMs) {
    ReceiveEvent event;
    event.id = id;
    event.index = index;
    event.name = name;
    event.bytes = bytes;
    event.channel = channel;
    event.timestampMs = timestampMs;
    m_receiveQueue.append(event);
    emit incomingWakeup();
}

void DLScriptEngine::onSendRequestDone() {
    m_sendDone = true;
    emit incomingWakeup();
}

// Runs the queued DL_OnReceive() events. Called from park slices, so the JS
// virtual machine is free (the main script is waiting inside a DL call).
void DLScriptEngine::processReceiveEvents() {
    while (!m_receiveQueue.isEmpty() && !m_stop) {
        m_onReceive = m_receiveQueue.takeFirst();
        const QJSValue handler = m_engine->globalObject().property("DL_OnReceive");
        if (handler.isCallable()) {
            QJSValue result = handler.call();
            if (result.isError()) {
                m_lastError = QStringLiteral("DL_OnReceive: %1")
                                  .arg(result.property("message").toString());
                m_stop = true;
                m_broken = true;
                if (m_engine)
                    m_engine->setInterrupted(true);
                m_receiveQueue.clear();
            }
        }
    }
}

QVariant DLScriptEngine::callBridge(const char *method, const QVariantList &args) {
    if (!m_bridge)
        return {};
    // One queue-safe dispatcher on the bridge; the typed script* methods stay
    // internal to the backend.
    QVariant result;
    const bool ok = QMetaObject::invokeMethod(m_bridge, "scriptInvoke", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(QVariant, result),
                              Q_ARG(QString, QLatin1String(method)),
                              Q_ARG(QVariantList, args));
    if (!ok)
        qWarning() << "callBridge: invoke failed for" << method;
    return result;
}

bool DLScriptEngine::waitUntil(const std::function<bool()> &predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (m_stop)
            return false;
        if (timeoutMs >= 0 && timeoutMs - int(timer.elapsed()) <= 0)
            return false;   // timed out; -1 means "wait without timeout"
        m_parked = true;
        QEventLoop loop;
        park(loop, this, timeoutMs < 0 ? -1 : timeoutMs - int(timer.elapsed()));
        m_parked = false;
        processReceiveEvents();
    }
    processReceiveEvents();
    return true;
}

void DLScriptEngine::run(QString code) {
    // Docklight scripts are VBScript; translate before running on the
    // JavaScript engine. Warnings about untranslated constructs surface in
    // the script output.
    QString effective = code;
    if (VbsTrans::looksLikeVbScript(effective)) {
        QStringList vbsWarnings;
        effective = VbsTrans::toJs(effective, &vbsWarnings);
        for (const QString &w : vbsWarnings)
            emit outputLine(QStringLiteral("[vbs] not translated: %1").arg(w), true);
        if (qEnvironmentVariableIsSet("LUCID_DUMP_VBS"))
            fprintf(stderr, "---- transpiled ----\n%s\n----------------\n",
                    effective.toUtf8().constData());
    }
    code = effective;
    m_stop = false;
    m_broken = false;
    m_quit = false;
    m_failed = false;
    m_failMessage.clear();
    m_startMs = QDateTime::currentMSecsSinceEpoch();
    m_lastError.clear();
    m_receiveQueue.clear();
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
    // VBScript-compatibility constants used by the Docklight samples.
    m_engine->evaluate(QStringLiteral(
        "var vbCrLf = '\\r\\n', vbCr = '\\r', vbLf = '\\n';"
        "var vbOK = 1, vbCancel = 2, vbAbort = 3, vbRetry = 4, vbIgnore = 5,"
        " vbYes = 6, vbNo = 7;"
        "var vbOKOnly = 0, vbOKCancel = 1, vbAbortRetryIgnore = 2,"
        " vbYesNoCancel = 3, vbYesNo = 4, vbRetryCancel = 5;"
        "var vbCritical = 16, vbQuestion = 32, vbExclamation = 48,"
        " vbInformation = 64;"));
    // FileInput / FileOutput, the Docklight file objects.
    m_engine->evaluate(QStringLiteral(
        "function FileInput(path) { return DL._openInput(path); }"
        "function FileOutput(path, append) { return DL._openOutput(path, append === true); }"));

    // VBScript runtime library: the date/time, string, conversion and math
    // functions the Docklight manual samples rely on.
    m_engine->evaluate(QStringLiteral(
        "var _DateCtor = Date;"
        "function _asDate(d) { if (d === undefined || d === null) return new _DateCtor();"
        "  if (d instanceof _DateCtor) return d; var p = new _DateCtor(d);"
        "  if (isNaN(p.getTime())) throw new Error('Type mismatch: not a date'); return p; }"
        "function Now() { return new _DateCtor(); }"
        "function _Date() { var d = new _DateCtor(); d.setHours(0, 0, 0, 0); return d; }"
        "function _Time() { return new _DateCtor(); }"
        "function VBDate() { return _Date(); }"
        "function VBTime() { return _Time(); }"
        "function Timer() { var d = new _DateCtor();"
        "  return d.getHours() * 3600 + d.getMinutes() * 60 + d.getSeconds()"
        "         + d.getMilliseconds() / 1000; }"
        "function Year(d) { return _asDate(d).getFullYear(); }"
        "function Month(d) { return _asDate(d).getMonth() + 1; }"
        "function Day(d) { return _asDate(d).getDate(); }"
        "function Hour(d) { return _asDate(d).getHours(); }"
        "function Minute(d) { return _asDate(d).getMinutes(); }"
        "function Second(d) { return _asDate(d).getSeconds(); }"
        "function DateSerial(y, m, d) { return new _DateCtor(y, m - 1, d); }"
        "function TimeSerial(h, m, s) { var d = new _DateCtor(); d.setHours(h, m, s, 0); return d; }"
        "function Weekday(d) { return _asDate(d).getDay() + 1; }"
        "var _dayNames = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];"
        "var _monthNames = ['January', 'February', 'March', 'April', 'May', 'June', 'July',"
        "                   'August', 'September', 'October', 'November', 'December'];"
        "function WeekdayName(w, abbrev) { var n = _dayNames[(w - 1) % 7];"
        "  return abbrev ? n.substr(0, 3) : n; }"
        "function MonthName(m, abbrev) { var n = _monthNames[(m - 1) % 12];"
        "  return abbrev ? n.substr(0, 3) : n; }"
        "function DateAdd(interval, number, date) { var d = _asDate(date); var n = Number(number);"
        "  switch (String(interval).toLowerCase()) {"
        "    case 'yyyy': d.setFullYear(d.getFullYear() + n); break;"
        "    case 'q': d.setMonth(d.getMonth() + 3 * n); break;"
        "    case 'm': d.setMonth(d.getMonth() + n); break;"
        "    case 'y': case 'd': case 'w': d.setDate(d.getDate() + n); break;"
        "    case 'ww': d.setDate(d.getDate() + 7 * n); break;"
        "    case 'h': d.setHours(d.getHours() + n); break;"
        "    case 'n': d.setMinutes(d.getMinutes() + n); break;"
        "    case 's': d.setSeconds(d.getSeconds() + n); break; }"
        "  return d; }"
        "function DateDiff(interval, d1, d2) { var a = _asDate(d1), b = _asDate(d2);"
        "  switch (String(interval).toLowerCase()) {"
        "    case 'yyyy': return b.getFullYear() - a.getFullYear();"
        "    case 'm': return (b.getFullYear() - a.getFullYear()) * 12 + b.getMonth() - a.getMonth();"
        "    case 'd': case 'y': case 'w': return Math.floor((b - a) / 86400000);"
        "    case 'ww': return Math.floor((b - a) / 604800000);"
        "    case 'h': return Math.floor((b - a) / 3600000);"
        "    case 'n': return Math.floor((b - a) / 60000);"
        "    case 's': return Math.floor((b - a) / 1000); }"
        "  return 0; }"
        "function FormatDateTime(d, style) { var p = _asDate(d);"
        "  var _2 = function (v) { return (v < 10 ? '0' : '') + v; };"
        "  style = style === undefined ? 0 : style;"
        "  var md = (p.getMonth() + 1) + '/' + p.getDate() + '/' + p.getFullYear();"
        "  var h12 = p.getHours() % 12; if (h12 === 0) h12 = 12;"
        "  var hm = _2(h12) + ':' + _2(p.getMinutes()) + ':' + _2(p.getSeconds())"
        "           + (p.getHours() < 12 ? ' AM' : ' PM');"
        "  if (style === 1) return p.toDateString();"
        "  if (style === 2) return md;"
        "  if (style === 3) return hm;"
        "  if (style === 4) return _2(p.getHours()) + ':' + _2(p.getMinutes());"
        "  return md + ' ' + hm; }"
        "function IsDate(v) { return v instanceof _DateCtor"
        "  || (typeof v === 'string' && !isNaN(new _DateCtor(v).getTime())); }"
        "function VBInt(x) { x = Number(x); return Math.floor(x); }"
        "function VBSgn(x) { x = Number(x); return x > 0 ? 1 : x < 0 ? -1 : 0; }"
        "function Left(s, n) { s = String(s); return n <= 0 ? '' : s.slice(0, Math.floor(n)); }"
        "function Right(s, n) { s = String(s); return n <= 0 ? '' : s.slice(Math.max(0, s.length - Math.floor(n))); }"
        "function Mid(s, start, len) { s = String(s); var b = Math.max(0, Math.floor(start) - 1);"
        "  return len === undefined ? s.slice(b) : s.slice(b, b + Math.max(0, Math.floor(len))); }"
        "function Len(s) { return String(s).length; }"
        "function InStr(a, b, c) {"
        "  if (typeof a === 'number') return String(b).indexOf(String(c), Math.max(0, a - 1)) + 1;"
        "  return String(a).indexOf(String(b)) + 1; }"
        "function InStrRev(s, needle) { return String(s).lastIndexOf(String(needle)) + 1; }"
        "function Replace(s, f, r) { s = String(s); f = String(f); r = String(r);"
        "  return f === '' ? s : s.split(f).join(r); }"
        "function UCase(s) { return String(s).toUpperCase(); }"
        "function LCase(s) { return String(s).toLowerCase(); }"
        "function Trim(s) { return String(s).replace(/^ +/, '').replace(/ +$/, ''); }"
        "function LTrim(s) { return String(s).replace(/^ +/, ''); }"
        "function RTrim(s) { return String(s).replace(/ +$/, ''); }"
        "function Space(n) { return new Array(Math.max(0, Math.floor(n)) + 1).join(' '); }"
        "function StrComp(a, b) { a = String(a); b = String(b); return a < b ? -1 : a > b ? 1 : 0; }"
        "function Asc(s) { return DL.Asc(s); }"
        "function Chr(c) { return DL.Chr(c); }"
        "function Hex(v) { return (Number(v) >>> 0).toString(16).toUpperCase(); }"
        "function Oct(v) { return (Number(v) >>> 0).toString(8).toUpperCase(); }"
        "function CStr(v) { if (v === true) return 'True'; if (v === false) return 'False';"
        "  return v === null || v === undefined ? '' : String(v); }"
        "function VBNum(v) { v = String(v); var t = v.replace(/^\\s+/, '');"
        "  if (/^&h/i.test(t)) return parseInt(t.slice(2), 16);"
        "  if (/^&o/i.test(t)) return parseInt(t.slice(2), 8);"
        "  var n = Number(t.replace(',', '.')); return isNaN(n) && t !== '' ? 0 : n; }"
        "function CInt(v) { return Math.round(VBNum(v)); }"
        "function CLng(v) { return CInt(v); }"
        "function CDbl(v) { return VBNum(v); }"
        "function CSng(v) { return CDbl(v); }"
        "function CBool(v) { return !!v; }"
        "function Round(v, digits) { var p = Math.pow(10, digits || 0);"
        "  return Math.round(Number(v) * p) / p; }"
        "function FormatNumber(v, digits) { return Number(v).toFixed(digits === undefined ? 2 : digits); }"
        "function Join(arr, sep) { return (arr || []).join(sep === undefined ? ' ' : sep); }"
        "function Split(s, sep) { return String(s).split(sep === undefined ? ' ' : sep); }"
        "function UBound(a) { return a.length - 1; }"
        "function LBound() { return 0; }"
        "var WScript = { Echo: function () { print.apply(null, arguments); } };"));

    const QJSValue result = m_engine->evaluate(code, QStringLiteral("script.js"));

    // Give queued DL_OnReceive events a final chance to run, like Docklight
    // which executes buffered events after the module level code ends.
    processReceiveEvents();

    QString error;
    bool ok = true;
    if (m_failed) {
        ok = false;
        error = m_failMessage;
    } else if (m_engine->isInterrupted()) {
        m_engine->setInterrupted(false);
        if (m_quit) {
            // DL.Quit ends the script like reaching its natural end.
        } else {
            ok = false;
            error = QStringLiteral("Script stopped by user");
        }
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

QByteArray DLScriptEngine::processOnSend(const QString &name, int index,
                                         const QByteArray &bytes) {
    const QJSValue handler = m_engine ? m_engine->globalObject().property("DL_OnSend")
                                      : QJSValue();
    if (!m_engine || !handler.isCallable())
        return bytes;
    m_onSendActive = true;
    m_onSendName = name;
    m_onSendIndex = index;
    m_onSendData = bytes;
    QJSValue result = handler.call();
    m_onSendActive = false;
    if (result.isError()) {
        m_lastError = QStringLiteral("DL_OnSend: %1")
                          .arg(result.property("message").toString());
        m_failed = true;
        m_failMessage = m_lastError;
        m_stop = true;
        if (m_engine)
            m_engine->setInterrupted(true);
        return bytes;
    }
    return m_onSendData;
}

void DLScriptEngine::onDialogResult(int result) {
    m_dialogResult = result;
    m_dialogDone = true;
    emit incomingWakeup();
}

void DLScriptEngine::onDialogText(const QString &text) {
    m_inputResult = text;
    m_inputReady = true;
    m_dialogDone = true;
    emit incomingWakeup();
}

// ---------------------------------------------------------------------------
// DLObject
// ---------------------------------------------------------------------------

DLObject::DLObject(DLScriptEngine *host) : QObject(host), m_host(host) {}

bool DLObject::StartCommunication() {
    if (!m_host->m_comm)
        return false;
    // Docklight: "Starts the communication, if not already running" — a
    // running session is left untouched (no restart, counters keep).
    if (m_host->m_comm->running())
        return true;
    m_host->callBridge("scriptStartCommunication", {});
    return m_host->m_comm->running();
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

void DLObject::ClearCommWindows() {
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
    return sendBytes(parsed.bytes());
}

bool DLObject::SendHex(const QString &hex) {
    const auto parsed = DataFormat::parse(hex, DataFormat::Format::Hex);
    if (!parsed.ok()) {
        m_host->m_lastError = parsed.error;
        return false;
    }
    return sendBytes(parsed.bytes());
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

// Docklight: DL.SendSequence nameOrIndex [, parameters] [, representation].
// Sends are posted to the main thread and the script parks until the send
// went out, so DL_OnSend() manipulation stays possible without deadlocks.
bool DLObject::SendSequence(const QJSValue &nameOrIndex, const QString &parameters,
                            const QString &representation) {
    const auto &entries = m_host->m_snapshot.entries;
    int row = -1;
    if (nameOrIndex.isNumber()) {
        const int index = nameOrIndex.toInt();
        if (index >= 0 && index < entries.size() && entries.at(index).isSend)
            row = index;
    } else {
        const QString name = nameOrIndex.toString();
        if (!name.isEmpty()) {
            for (int i = 0; i < entries.size(); ++i)
                if (entries.at(i).isSend
                    && entries.at(i).name.compare(name, Qt::CaseInsensitive) == 0) {
                    row = i;
                    break;
                }
        }
    }
    if (nameOrIndex.toString().isEmpty() && !nameOrIndex.isNumber()) {
        // Syntax 2: custom sequence data.
        m_host->m_sendDone = false;
        emit m_host->sendRequest(-1, parameters, representation, QStringList());
        m_host->waitUntil([this] { return m_host->m_sendDone; }, 30000);
        return true;
    }
    if (row < 0) {
        m_host->m_lastError =
            QStringLiteral("Send sequence not found: %1").arg(nameOrIndex.toString());
        return false;
    }
    m_host->m_sendDone = false;
    // Docklight splits the parameter string into one value per wildcard
    // area: separated by spaces for ASCII, by commas for HEX/DEC/BIN.
    QStringList paramList;
    if (!parameters.isEmpty()) {
        const bool hexLike = representation.compare(QLatin1String("A"),
                                                    Qt::CaseInsensitive) != 0;
        paramList = hexLike ? parameters.split(',', Qt::SkipEmptyParts)
                            : parameters.split(' ', Qt::SkipEmptyParts);
        for (QString &p : paramList)
            p = p.trimmed();
    }
    emit m_host->sendRequest(row, QString(), representation, paramList);
    m_host->waitUntil([this] { return m_host->m_sendDone; }, 30000);
    return true;
}

// DL.DirectSend channelNo, customSequence [, representation] — transmits
// without communication window output and without the OnSend queue.
bool DLObject::DirectSend(int channelNo, const QString &customSequence,
                          const QString &representation) {
    if (!m_host->m_comm)
        return false;
    DataFormat::Format format = DataFormat::Format::Text;
    if (representation.compare(QLatin1String("H"), Qt::CaseInsensitive) == 0)
        format = DataFormat::Format::Hex;
    else if (representation.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0)
        format = DataFormat::Format::Dec;
    const auto parsed = DataFormat::parse(customSequence, format);
    if (!parsed.ok()) {
        m_host->m_lastError = parsed.error;
        return false;
    }
    if (channelNo >= 3 && channelNo <= 10) {
        bool sent = false;
        QMetaObject::invokeMethod(m_host->m_comm, "sideSend", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(bool, sent), Q_ARG(int, channelNo),
                                  Q_ARG(QByteArray, parsed.bytes()));
        return sent;
    }
    const int channel = qBound(1, channelNo, 2) - 1;
    // DirectSend bypasses the OnSend queue AND the communication window
    // output (no TX row), unlike SendSequence.
    bool sent = false;
    QMetaObject::invokeMethod(m_host->m_comm, "sendOnEx", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, sent), Q_ARG(int, channel),
                              Q_ARG(QByteArray, parsed.bytes()), Q_ARG(bool, false));
    return sent;
}

// DL.OpenSideChannel channelNo, device — side channels 3..10.
bool DLObject::OpenSideChannel(int channelNo, const QString &device) {
    if (!m_host->m_comm || channelNo < 3 || channelNo > 10)
        return false;
    bool opened = false;
    QMetaObject::invokeMethod(m_host->m_comm, "openSideChannel", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, opened), Q_ARG(int, channelNo),
                              Q_ARG(QString, device));
    return opened;
}

void DLObject::CloseSideChannel(int channelNo) {
    if (!m_host->m_comm)
        return;
    QMetaObject::invokeMethod(m_host->m_comm, "closeSideChannel", Qt::BlockingQueuedConnection,
                              Q_ARG(int, channelNo));
}

void DLObject::SendBreak(int durationMs) {
    if (!m_host->m_comm)
        return;
    QMetaObject::invokeMethod(m_host->m_comm, "sendBreak", Qt::QueuedConnection, Q_ARG(int, durationMs));
}

void DLObject::SetHandshakeSignals(bool rts, bool dtr) {
    if (!m_host->m_comm)
        return;
    QMetaObject::invokeMethod(m_host->m_comm, "setRTS", Qt::QueuedConnection, Q_ARG(int, 0),
                              Q_ARG(bool, rts));
    QMetaObject::invokeMethod(m_host->m_comm, "setDTR", Qt::QueuedConnection, Q_ARG(int, 0),
                              Q_ARG(bool, dtr));
}

int DLObject::GetHandshakeSignals() {
    if (!m_host->m_comm)
        return 0;
    const CommLink *link = m_host->m_comm->link(0);
    return (link->cts() ? 1 : 0) | (link->dsr() ? 2 : 0) | (link->dcd() ? 4 : 0)
           | (link->ri() ? 8 : 0);
}

bool DLObject::GetCTS() {
    return m_host->m_comm && m_host->m_comm->link(0)->cts();
}
bool DLObject::GetDSR() {
    return m_host->m_comm && m_host->m_comm->link(0)->dsr();
}
bool DLObject::GetDCD() {
    return m_host->m_comm && m_host->m_comm->link(0)->dcd();
}
bool DLObject::GetRI() {
    return m_host->m_comm && m_host->m_comm->link(0)->ri();
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
    QByteArray wanted;
    QVector<bool> wildcard;
    for (const auto &e : parsed.elements) {
        if (e.kind == ParsedBytes::Element::Data
            || e.kind == ParsedBytes::Element::WildOne
            || e.kind == ParsedBytes::Element::WildZeroOrOne) {
            wanted.append(char(e.value));
            wildcard.append(e.kind != ParsedBytes::Element::Data);
        }
    }
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

// Docklight: result = DL.WaitForSequence(nameOrIndex [, maxCounter]
// [, timeout]) — waits until the sequence's hit counter reaches maxCounter
// (default 1) or the timeout passes, and returns the counter value.
int DLObject::WaitForSequence(const QJSValue &nameOrIndex, int maxCounter, int timeout) {
    // Resolve the target sequence and wait on its pattern in the local
    // stream buffer as well, so a fresh match is never missed even when the
    // counter has not propagated yet.
    const auto &entries = m_host->m_snapshot.entries;
    const DLScriptEngine::Snapshot::Entry *entry = nullptr;
    int index = -1;
    if (nameOrIndex.isNumber()) {
        index = nameOrIndex.toInt();
        if (index >= 0 && index < entries.size() && !entries.at(index).isSend)
            entry = &entries.at(index);
    } else {
        const QString name = nameOrIndex.toString();
        for (int i = 0; i < entries.size(); ++i)
            if (!entries.at(i).isSend
                && entries.at(i).name.compare(name, Qt::CaseInsensitive) == 0) {
                entry = &entries.at(i);
                index = i;
                break;
            }
    }
    if (!entry) {
        m_host->m_lastError =
            QStringLiteral("Receive sequence not found: %1").arg(nameOrIndex.toString());
        return 0;
    }
    // Docklight: WaitForSequence "starts the communication, if not already
    // running".
    if (!m_host->m_comm->running())
        StartCommunication();
    // Snapshot receive sequences carry their id; resolve the counter through
    // the bridge (main thread owns the counters).
    int seqId = entry->id;
    // Docklight counts hits since communication start or the last counter
    // reset, so maxCounter is an absolute limit, not a delta.
    const int target = maxCounter;

    const QByteArray bytes = entry->bytes;
    const QVector<bool> wildcard = entry->wildcard;
    int hits = 0;
    auto localSearch = [&]() -> int {
        const int size = bytes.size();
        if (size == 0)
            return 0;
        const bool hasMask = wildcard.size() == size;
        QMutexLocker lock(&m_host->m_streamMutex);
        const char *data = m_host->m_stream.constData();
        int consumed = 0;
        while (m_host->m_stream.size() - consumed >= size) {
            bool found = false;
            for (int at = 0; at + size <= m_host->m_stream.size() - consumed; ++at) {
                bool ok = true;
                for (int i = 0; i < size; ++i)
                    if ((!hasMask || !wildcard.at(i)) && data[at + i] != bytes.at(i)) {
                        ok = false;
                        break;
                    }
                if (ok) {
                    found = true;
                    consumed += at + size;
                    break;
                }
            }
            if (!found)
                break;
            ++hits;
        }
        if (consumed > 0)
            m_host->m_stream.remove(0, consumed);
        return hits;
    };

    m_host->waitUntil(
        [&]() {
            localSearch();
            if (!m_host->m_bridge)
                return target < 0 ? false : hits >= target;
            const int count = m_host->callBridge("scriptReceiveCount", {seqId}).toInt();
            return target < 0 ? false : count >= target;
        },
        timeout);
    int count = hits;
    if (m_host->m_bridge)
        count = qMax(count, m_host->callBridge("scriptReceiveCount", {seqId}).toInt());
    return count;
}

QJSValue DLObject::ReceiveLine(int timeoutMs) {
    const bool arrived = m_host->waitUntil(
        [this]() {
            QMutexLocker lock(&m_host->m_streamMutex);
            return m_host->m_stream.indexOf('\n') >= 0;
        },
        timeoutMs);
    if (!arrived || m_host->m_stop) {
        m_host->m_lastError = QStringLiteral("Receive timed out waiting for a line");
        return QJSValue(false);
    }
    QByteArray line;
    {
        QMutexLocker lock(&m_host->m_streamMutex);
        const int at = m_host->m_stream.indexOf('\n');
        line = m_host->m_stream.left(at);
        m_host->m_stream.remove(0, at + 1);
    }
    if (line.endsWith('\r'))
        line.chop(1);
    return m_host->jsEngine()->toScriptValue(QString::fromUtf8(line));
}

int DLObject::GetReceiveCounter(const QJSValue &nameOrIndex) {
    const int seqId = resolveSequenceId(nameOrIndex, false);
    if (seqId < 0)
        return 0;
    return m_host->callBridge("scriptReceiveCount", {seqId}).toInt();
}

void DLObject::ResetReceiveCounter(const QJSValue &nameOrIndex) {
    const int seqId = nameOrIndex.isUndefined() || nameOrIndex.isNull()
                          ? -1
                          : resolveSequenceId(nameOrIndex, false);
    m_host->callBridge("scriptResetCounters", {seqId});
}

QString DLObject::GetReceiveComments() {
    return m_host->callBridge("scriptDrainReceiveComments").toString();
}

int DLObject::NoOfSendSequences() const {
    int count = 0;
    for (const auto &entry : m_host->m_snapshot.entries)
        if (entry.isSend)
            ++count;
    return count;
}

int DLObject::NoOfReceiveSequences() const {
    int count = 0;
    for (const auto &entry : m_host->m_snapshot.entries)
        if (!entry.isSend)
            ++count;
    return count;
}

int DLObject::resolveSequenceId(const QJSValue &nameOrIndex, bool send) const {
    const auto &entries = m_host->m_snapshot.entries;
    if (nameOrIndex.isNumber()) {
        const int index = nameOrIndex.toInt();
        if (index >= 0 && index < entries.size() && entries.at(index).isSend == send)
            return entries.at(index).id;
        return -1;
    }
    const QString name = nameOrIndex.toString();
    for (const auto &entry : entries)
        if (entry.isSend == send && entry.name.compare(name, Qt::CaseInsensitive) == 0)
            return entry.id;
    return -1;
}

// Docklight: DL.AddComment [comment] [, timeStampAfterComment]
// [, lineBreakAndPadding]
void DLObject::AddComment(const QString &comment, bool timeStampAfterComment,
                          bool lineBreakAndPadding) {
    QString text = comment;
    if (timeStampAfterComment)
        text += QStringLiteral(" ") + GetDocklightTimeStamp(QJSValue(), -1, true);
    // lineBreakAndPadding=false lets the next AddComment call continue the
    // same output line (CommLog keeps the row open until it is closed).
    emit m_host->commentRequested(text, !lineBreakAndPadding);
}

QString DLObject::GetCommWindowData(const QString &representation) {
    return m_host->callBridge("scriptCommWindowData", {representation}).toString();
}

void DLObject::SetContentsFilter(int filter) {
    m_host->callBridge("scriptSetContentsFilter", {filter});
}

// Docklight: result = DL.GetDocklightTimeStamp([dateTime] [, millis]
// [, trimmed]) — with the trailing space kept by default for compatibility.
QString DLObject::GetDocklightTimeStamp(const QJSValue &dateTime, int milliseconds,
                                        bool trimmed) {
    QDateTime stamp = QDateTime::currentDateTime();
    if (dateTime.isNumber() && dateTime.toNumber() > 0)
        stamp.setSecsSinceEpoch(qint64(dateTime.toNumber()));
    QString text;
    if (milliseconds == -2)
        text = stamp.toString(QStringLiteral("MM/dd/yyyy HH:mm:ss"));
    else {
        int ms = milliseconds;
        if (ms < 0)
            ms = stamp.time().msec();
        text = stamp.toString(QStringLiteral("MM/dd/yyyy HH:mm:ss"))
               + QString(".%1").arg(ms, 3, 10, QChar('0'));
    }
    if (!trimmed)
        text += QLatin1Char(' ');   // Docklight keeps a trailing space here
    return text;
}

void DLObject::Wait(int ms) {
    m_host->waitUntil([] { return false; }, qMax(0, ms));
}

void DLObject::Pause(int ms) {
    QEventLoop loop;
    QTimer::singleShot(qMax(0, ms), &loop, &QEventLoop::quit);
    loop.exec();
}

void DLObject::Break() {
    m_host->m_broken = true;
    m_host->m_stop = true;
    if (m_host->jsEngine())
        m_host->jsEngine()->setInterrupted(true);
}

void DLObject::Quit() {
    // Quit ends the script normally: stop the engine with the quit flag so
    // the run reports success, like a script reaching its natural end.
    m_host->m_quit = true;
    m_host->m_stop = true;
    if (m_host->jsEngine())
        m_host->jsEngine()->setInterrupted(true);
}

void DLObject::Fail(const QString &message) {
    m_host->m_failed = true;
    m_host->m_failMessage = message.isEmpty() ? QStringLiteral("Test failed") : message;
    m_host->m_stop = true;
    if (m_host->jsEngine())
        m_host->jsEngine()->setInterrupted(true);
}

void DLObject::Assert(const QJSValue &condition, const QString &message) {
    if (!condition.toBool())
        Fail(message.isEmpty() ? QStringLiteral("Assertion failed") : message);
}

QString DLObject::Env(const QString &name, const QString &fallback) {
    const QString value = qEnvironmentVariable(name.toUtf8().constData());
    return value.isEmpty() ? fallback : value;
}

void DLObject::SaveLog(const QString &path) {
    if (!path.isEmpty())
        emit m_host->saveLogRequested(path);
}

// ---------------------------------------------------------------------------
// Checksums and conversions
// ---------------------------------------------------------------------------

QString DLObject::CalcChecksum(const QString &checksumSpec, const QString &dataStr,
                               const QString &representation, int startPos, int endPos,
                               bool bigEndian) {
    QByteArray data;
    if (representation.compare(QLatin1String("A"), Qt::CaseInsensitive) == 0)
        data = dataStr.toLatin1();
    else {
        DataFormat::Format format = DataFormat::Format::Hex;
        if (representation.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0
            || representation.compare(QLatin1String("B"), Qt::CaseInsensitive) == 0)
            format = DataFormat::Format::Dec;
        const auto parsed = DataFormat::parse(dataStr, format);
        if (!parsed.ok()) {
            m_host->m_lastError = parsed.error;
            return QString();
        }
        data = parsed.bytes();
    }
    // Docklight positions are 1-based; negative values count from the end.
    int start = 0;
    if (startPos > 0)
        start = startPos - 1;
    else if (startPos < 0)
        start = data.size() + startPos;
    int length = data.size() - start;
    if (endPos > 0)
        length = endPos - startPos + 1;
    else if (endPos < 0)
        length = (data.size() + endPos) - start + 1;
    if (start < 0 || length <= 0 || start + length > data.size()) {
        m_host->m_lastError = QStringLiteral("Checksum area out of range");
        return QString();
    }
    QByteArray result = Checksum::compute(checksumSpec, data.mid(start, length));
    if (result.isEmpty()) {
        m_host->m_lastError = QStringLiteral("Unknown checksum specification");
        return QString();
    }
    if (!bigEndian && representation.compare(QLatin1String("A"), Qt::CaseInsensitive) != 0)
        std::reverse(result.begin(), result.end());
    return DataFormat::toHex(result);
}

// VBScript-style number output, as Docklight's ConvertSequenceData shows it:
// up to 7 significant digits for single precision, 15 for double, trailing
// zeros trimmed, scientific notation with an uppercase E and a signed
// two-digit exponent outside the fixed range.
namespace {
QString vbNumber(double value, bool single) {
    if (qIsNaN(value))
        return QStringLiteral("NaN");
    if (qIsInf(value))
        return value < 0 ? QStringLiteral("-Inf") : QStringLiteral("Inf");
    if (value == 0)
        return QStringLiteral("0");
    const int digits = single ? 7 : 15;
    const double abs = qAbs(value);
    const int exponent = int(std::floor(std::log10(abs)));
    if (exponent >= -4 && exponent < digits) {
        QString text = QString::number(value, 'g', digits);
        // 'g' may still produce an exponent near the switch boundary.
        if (!text.contains('e'))
            return text;
    }
    QString text = QString::number(value, 'e', digits - 1);
    const int ePos = text.indexOf('e');
    QString mantissa = text.left(ePos);
    QString exponentText = text.mid(ePos + 1);
    if (mantissa.contains('.')) {
        while (mantissa.endsWith('0'))
            mantissa.chop(1);
        if (mantissa.endsWith('.'))
            mantissa.chop(1);
    }
    // Two-digit exponent minimum with an explicit sign, like VBScript.
    bool negativeExp = exponentText.startsWith('-');
    QString digits2 = exponentText;
    digits2.remove('-').remove('+');
    while (digits2.size() < 2)
        digits2.prepend('0');
    return mantissa + (negativeExp ? QStringLiteral("E-") : QStringLiteral("E+")) + digits2;
}
}  // namespace

QString DLObject::ConvertSequenceData(const QString &conversionType, const QString &source,
                                      const QString &representation, bool bigEndian) {
    // Result format: "H" hex (default), "D" decimal, "B" binary.
    const auto formatOf = [&](QByteArray data) {
        DataFormat::Format f = DataFormat::Format::Hex;
        if (representation.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0)
            f = DataFormat::Format::Dec;
        else if (representation.compare(QLatin1String("B"), Qt::CaseInsensitive) == 0)
            f = DataFormat::Format::Bin;
        if (f == DataFormat::Format::Dec) {
            // Docklight pads decimal sequence bytes to three digits
            // ("103 154 149 160 081 161 036 075").
            QStringList parts;
            for (uchar byte : data)
                parts.append(QString::asprintf("%03u", byte));
            return parts.join(QLatin1Char(' '));
        }
        return DataFormat::display(data, f);
    };
    const auto hexOf = [&](quint64 value, int bytes, bool littleEndian) {
        QByteArray out;
        for (int i = 0; i < bytes; ++i)
            out.append(char((value >> (8 * (littleEndian ? i : bytes - 1 - i))) & 0xFF));
        return formatOf(out);
    };
    const auto bytesOf = [&](const QString &text) {
        DataFormat::Format format = DataFormat::Format::Hex;
        if (representation.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0)
            format = DataFormat::Format::Dec;
        else if (representation.compare(QLatin1String("B"), Qt::CaseInsensitive) == 0)
            format = DataFormat::Format::Bin;
        const auto parsed = DataFormat::parse(text, format);
        return parsed.bytes();
    };
    const auto numberFrom = [&](const QByteArray &bytes, int size, bool signedValue) {
        quint64 value = 0;
        for (int i = 0; i < qMin(size, bytes.size()); ++i) {
            const uchar byte = uchar(bytes.at(bigEndian ? i : size - 1 - i));
            value = (value << 8) | byte;
        }
        if (signedValue) {
            const quint64 signBit = quint64(1) << (size * 8 - 1);
            if (value & signBit)
                return double(value) - double(quint64(1) << (size * 8));
        }
        return double(value);
    };
    const QString type = conversionType.toLower();
    const QByteArray bytes = bytesOf(source);
    if (type == "tosingle") {
        float f = 0;
        if (bytes.size() >= 4) {
            quint32 v = 0;
            for (int i = 0; i < 4; ++i)
                v = (v << 8) | uchar(bytes.at(bigEndian ? i : 3 - i));
            memcpy(&f, &v, 4);
        }
        return vbNumber(double(f), true);
    }
    if (type == "fromsingle") {
        float f = QString(source).replace(',', '.').toFloat();
        quint32 v = 0;
        memcpy(&v, &f, 4);
        return hexOf(v, 4, !bigEndian);
    }
    if (type == "todouble") {
        double d = 0;
        if (bytes.size() >= 8) {
            quint64 v = 0;
            for (int i = 0; i < 8; ++i)
                v = (v << 8) | uchar(bytes.at(bigEndian ? i : 7 - i));
            memcpy(&d, &v, 8);
        }
        return vbNumber(d, false);
    }
    if (type == "fromdouble") {
        double d = QString(source).replace(',', '.').toDouble();
        quint64 v = 0;
        memcpy(&v, &d, 8);
        return hexOf(v, 8, !bigEndian);
    }
    if (type == "fromtext") {
        QByteArray encoded = source.toLatin1();
        if (!bigEndian) {
            // Docklight: little endian means "no separator".
            return DataFormat::toHex(encoded).remove(' ');
        }
        return DataFormat::toHex(encoded);
    }
    if (type == "totext") {
        QString text;
        for (uchar byte : bytes)
            if (byte >= 32 && byte <= 126)
                text += QChar(byte);
        return text;
    }
    if (type == "tointeger16")
        return QString::number(qint64(numberFrom(bytes, 2, true)));
    if (type == "frominteger16") {
        const int v = source.toInt();
        return hexOf(quint16(v), 2, !bigEndian);
    }
    if (type == "tounsigned16")
        return QString::number(quint64(numberFrom(bytes, 2, false)), 'f', 0);
    if (type == "fromunsigned16") {
        const uint v = source.toUInt();
        return hexOf(quint16(v), 2, !bigEndian);
    }
    if (type == "tointeger32")
        return QString::number(qint64(numberFrom(bytes, 4, true)));
    if (type == "frominteger32") {
        const int v = source.toInt();
        return hexOf(quint32(v), 4, !bigEndian);
    }
    if (type == "tounsigned32")
        return QString::number(quint64(numberFrom(bytes, 4, false)), 'f', 0);
    if (type == "fromunsigned32") {
        const uint v = source.toUInt();
        return hexOf(quint32(v), 4, !bigEndian);
    }
    if (type == "tobool")
        return !bytes.isEmpty() && bytes.at(0) != 0 ? QStringLiteral("True")
                                                    : QStringLiteral("False");
    m_host->m_lastError = QStringLiteral("Unknown conversion type %1").arg(conversionType);
    return QString();
}

// ---------------------------------------------------------------------------
// Channels, project, files, dialogs
// ---------------------------------------------------------------------------

QString DLObject::GetChannelSettings(int channelNo) {
    return m_host->callBridge("scriptChannelSettings", {channelNo}).toString();
}

bool DLObject::SetChannelSettings(const QString &newSettings, int channelNo, bool dontTest) {
    Q_UNUSED(dontTest)
    return m_host->callBridge("scriptSetChannelSettings", {newSettings, channelNo}).toBool();
}

int DLObject::GetChannelStatus(int channelNo) {
    return m_host->callBridge("scriptChannelStatus", {channelNo}).toInt();
}

QString DLObject::GetEnvironment(const QString &name) {
    // Docklight-specific variables, exposed under their original names and
    // the LUCIDLIGHT_ aliases.
    const QString upper = name.toUpper();
    if (upper == "DOCKLIGHT_VERSION" || upper == "LUCIDLIGHT_VERSION")
        return QStringLiteral("Lucidlight V0.2 (Docklight Scripting compatible)");
    if (upper == "DOCKLIGHT_SCRIPTDIR" || upper == "LUCIDLIGHT_SCRIPTDIR")
        return QFileInfo::exists(QStringLiteral("script.js")) ? QStringLiteral(".") : QStringLiteral(".");
    if (upper == "DOCKLIGHT_PORTLIST" || upper == "LUCIDLIGHT_PORTLIST")
        return m_host->callBridge("scriptPortList").toString();
    if (upper.startsWith("DOCKLIGHT_SENDSEQDEF") || upper.startsWith("LUCIDLIGHT_SENDSEQDEF"))
        return QStringLiteral("");
    if (upper == "DOCKLIGHT_SENDSEQ" || upper == "LUCIDLIGHT_SENDSEQ") {
        QStringList names;
        for (const auto &entry : m_host->m_snapshot.entries)
            if (entry.isSend)
                names << entry.name;
        return names.join(QLatin1Char('\n'));
    }
    if (upper == "DOCKLIGHT_RECEIVESEQ" || upper == "LUCIDLIGHT_RECEIVESEQ") {
        QStringList names;
        for (const auto &entry : m_host->m_snapshot.entries)
            if (!entry.isSend)
                names << entry.name;
        return names.join(QLatin1Char('\n'));
    }
    const QString value = qEnvironmentVariable(name.toUtf8().constData());
    return value;
}

int DLObject::GetKeyState(int virtKey) {
    Q_UNUSED(virtKey)
    // Keyboard state inspection is not portable; Lucidlight reports "not
    // pressed" (0). Documented deviation from the Windows original.
    return 0;
}

void DLObject::OpenProject(const QString &filePathName) {
    if (filePathName.trimmed().isEmpty())
        return;
    if (!m_host->callBridge("scriptOpenProject", {filePathName}).toBool()) {
        m_host->m_lastError = QStringLiteral("Could not open project %1").arg(filePathName);
        Fail(m_host->m_lastError);
    }
}

void DLObject::LoadProgramOptions(const QString &filePathName) {
    // Options live with the project; loading a companion options file
    // restores the display settings saved by SaveProgramOptions.
    const QString path = filePathName + QStringLiteral(".options.json");
    if (!m_host->callBridge("scriptLoadOptions", {path}).toBool())
        m_host->m_lastError = QStringLiteral("Could not load program options");
}

void DLObject::SaveProgramOptions(const QString &filePathName) {
    const QString path = filePathName + QStringLiteral(".options.json");
    m_host->callBridge("scriptSaveOptions", {path});
}

bool DLObject::StartLogging(const QString &baseFilePath, bool appendData,
                            const QString &representations, int format, bool highspeed,
                            bool noHeaders) {
    Q_UNUSED(highspeed)
    return m_host->callBridge("scriptStartLogging",
                              {baseFilePath, appendData, representations, format, noHeaders})
        .toBool();
}

void DLObject::StopLogging() {
    m_host->callBridge("scriptStopLogging");
}

// Plays back a Docklight-style hex log file: lines with a time stamp, a
// [TX]/[RX] tag and hex bytes. Timing follows the original deltas unless a
// fixed interval is given.
void DLObject::PlaybackLogFile(const QString &filePathName, const QString &dataDirection,
                               int timeInterval) {
    QFile file(filePathName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Fail(QStringLiteral("Could not open log file %1").arg(filePathName));
        return;
    }
    struct Entry { qint64 at; QByteArray bytes; };
    QVector<Entry> entries;
    qint64 base = 0;
    const QRegularExpression lineRe(
        QStringLiteral("^(\\d+)/(\\d+)/(\\d+) (\\d+):(\\d+):(\\d+)\\.(\\d+) \\[(TX|RX)(\\d*)\\] - ([0-9A-F ]+)$"));
    while (!file.atEnd()) {
        const QString line = QString::fromLatin1(file.readLine()).trimmed();
        const auto m = lineRe.match(line);
        if (!m.hasMatch())
            continue;
        if (!dataDirection.isEmpty()
            && m.captured(8).compare(dataDirection, Qt::CaseInsensitive) != 0)
            continue;
        const QDateTime stamp(QDate(m.captured(3).toInt(), m.captured(1).toInt(),
                                    m.captured(2).toInt()),
                              QTime(m.captured(4).toInt(), m.captured(5).toInt(),
                                    m.captured(6).toInt(), m.captured(7).toInt() * 10));
        const qint64 at = stamp.isValid() ? stamp.toMSecsSinceEpoch() : 0;
        if (base == 0)
            base = at;
        entries.append({at - base, DataFormat::fromHex(m.captured(10))});
    }
    qint64 previous = 0;
    for (const Entry &entry : entries) {
        const int wait = timeInterval > 0 ? timeInterval : int(entry.at - previous);
        if (wait > 0)
            m_host->waitUntil([] { return false; }, wait);
        previous = timeInterval > 0 ? previous + timeInterval : entry.at;
        sendBytes(entry.bytes);
    }
}

void DLObject::UploadFile(const QString &filePathName, const QString &representation) {
    QFile file(filePathName);
    if (!file.open(QIODevice::ReadOnly)) {
        Fail(QStringLiteral("Could not open file %1").arg(filePathName));
        return;
    }
    const QString rep = representation.toUpper();
    if (rep == QLatin1String("R")) {
        // Raw binary, 512 byte blocks like Docklight.
        while (!file.atEnd() && !m_host->m_stop) {
            const QByteArray block = file.read(512);
            sendBytes(block);
        }
        return;
    }
    const QByteArray content = file.readAll();
    QString text = QString::fromLatin1(content);
    if (rep == QLatin1String("H") || rep == QLatin1String("D") || rep == QLatin1String("B")) {
        DataFormat::Format format = rep == QLatin1String("H") ? DataFormat::Format::Hex
                                                              : DataFormat::Format::Dec;
        const auto parsed = DataFormat::parse(text, format);
        if (!parsed.ok()) {
            Fail(parsed.error);
            return;
        }
        sendBytes(parsed.bytes());
        return;
    }
    // "A": send the text file as-is, in blocks.
    for (int at = 0; at < text.size() && !m_host->m_stop; at += 512)
        sendBytes(text.mid(at, 512).toUtf8());
}

int DLObject::ShellRun(const QString &operation, const QString &file,
                       const QString &parameters, const QString &directory, int showCmd) {
    Q_UNUSED(operation)
    Q_UNUSED(showCmd)
    if (QProcess::startDetached(file, parameters.split(' ', Qt::SkipEmptyParts),
                                directory.isEmpty() ? QDir::currentPath() : directory))
        return 42;   // > 32: Docklight reports the HINSTANCE, a success marker
    return -1;
}

int DLObject::MsgBox2(const QString &prompt, int buttons, const QString &title) {
    m_host->m_dialogDone = false;
    emit m_host->dialogRequest(prompt, buttons, title, false, QString());
    m_host->waitUntil([this] { return m_host->m_dialogDone; }, -1);
    return m_host->m_dialogResult;
}

QString DLObject::InputBox2(const QString &prompt, const QString &title,
                            const QString &defaultText) {
    m_host->m_dialogDone = false;
    m_host->m_inputReady = false;
    m_host->m_inputResult = defaultText;
    emit m_host->dialogRequest(prompt, 0, title, true, defaultText);
    m_host->waitUntil([this] { return m_host->m_dialogDone; }, -1);
    if (!m_host->m_inputReady)
        m_host->m_inputResult = defaultText;
    return m_host->m_inputResult;
}

// Minimal RTF to plain text: drops control words and group braces, keeps
// the readable characters, so RTF documents stay legible in the output pane.
namespace {
QString stripRtf(const QString &rtf) {
    QString out;
    bool escaping = false;
    for (int i = 0; i < rtf.size(); ++i) {
        const QChar c = rtf.at(i);
        if (escaping) {
            if (c.isSpace()) {
                escaping = false;
            } else if (c.isLetterOrNumber() || c == '\'' || c == '-') {
                continue;
            } else {
                if (c == '\\') {
                    // One control word ends where the next begins; do not
                    // emit the backslash itself.
                    escaping = true;
                    continue;
                }
                escaping = false;
                if (c != '*')
                    out.append(c);
            }
            continue;
        }
        if (c == '\\') {
            escaping = true;
        } else if (c == '{' || c == '}') {
            continue;
        } else {
            out.append(c);
        }
    }
    return out;
}
}  // namespace

void DLObject::SetUserOutput(const QString &text, bool rtfFormat, bool append,
                             bool readFromFile) {
    QString content = text;
    if (rtfFormat && !readFromFile)
        content = stripRtf(content);
    if (readFromFile) {
        QFile file(text);
        content = file.open(QIODevice::ReadOnly | QIODevice::Text)
                      ? QString::fromUtf8(file.readAll())
                      : QString();
        if (rtfFormat)
            content = stripRtf(content);
    }
    if (!append)
        emit m_host->outputLine(QStringLiteral("\x01" "CLEAR"), false);
    if (!content.isEmpty()) {
        const QStringList lines = content.split('\n');
        for (const QString &line : lines)
            emit m_host->outputLine(line.trimmed().remove('\r'), false);
    }
}

void DLObject::SetWindowLayout(const QString &layout) {
    m_host->callBridge("scriptSetWindowLayout", {layout});
}

// ---------------------------------------------------------------------------
// DL_OnSend() / DL_OnReceive() context
// ---------------------------------------------------------------------------

namespace {
QString contextData(const QByteArray &bytes, const QString &representation, int start,
                    int length) {
    QByteArray slice = bytes;
    if (start > 0 || length >= 0) {
        int begin = start > 0 ? start - 1 : bytes.size() + start;
        int end = length < 0 ? bytes.size() + length + 1 : begin + length;
        begin = qBound(0, begin, bytes.size());
        end = qBound(0, end, bytes.size());
        if (end < begin)
            end = begin;
        slice = bytes.mid(begin, end - begin);
    }
    if (representation.compare(QLatin1String("H"), Qt::CaseInsensitive) == 0)
        return DataFormat::toHex(slice);
    if (representation.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0)
        return DataFormat::display(slice, DataFormat::Format::Dec);
    if (representation.compare(QLatin1String("B"), Qt::CaseInsensitive) == 0) {
        QString bin;
        for (uchar byte : slice) {
            if (!bin.isEmpty())
                bin += QLatin1Char(' ');
            bin += QString("%1").arg(byte, 8, 2, QChar('0'));
        }
        return bin;
    }
    return QString::fromLatin1(slice);
}
}  // namespace

int DLObject::OnSend_GetSize() const {
    return m_host->m_onSendData.size();
}
QString DLObject::OnSend_GetName() const {
    return m_host->m_onSendName;
}
int DLObject::OnSend_GetIndex() const {
    return m_host->m_onSendIndex;
}
QString DLObject::OnSend_GetData(const QString &representation, int start, int length) {
    return contextData(m_host->m_onSendData, representation, start, length);
}
void DLObject::OnSend_SetData(const QString &newData, const QString &representation) {
    if (representation.compare(QLatin1String("H"), Qt::CaseInsensitive) == 0)
        m_host->m_onSendData = DataFormat::fromHex(newData);
    else if (representation.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0
             || representation.compare(QLatin1String("B"), Qt::CaseInsensitive) == 0)
        m_host->m_onSendData = DataFormat::parse(newData, DataFormat::Format::Dec).bytes();
    else
        m_host->m_onSendData = newData.toLatin1();
}
QJSValue DLObject::OnSend_Peek(int charNo, const QString &representation) {
    const int index = charNo > 0 ? charNo - 1 : m_host->m_onSendData.size() + charNo;
    if (index < 0 || index >= m_host->m_onSendData.size())
        return QJSValue(0);
    const uchar value = uchar(m_host->m_onSendData.at(index));
    if (!representation.isEmpty()) {
        // Docklight returns a STRING in the requested representation:
        // "A" = ASCII, "H" = HEX, "D" = Decimal, "B" = Binary.
        const QChar r = representation.at(0).toUpper();
        if (r == 'H')
            return QJSValue(QStringLiteral("%1").arg(value, 2, 16, QChar('0')).toUpper());
        if (r == 'D')
            return QJSValue(QStringLiteral("%1").arg(value, 3, 10, QChar('0')));
        if (r == 'B')
            return QJSValue(QStringLiteral("%1").arg(value, 8, 2, QChar('0')));
        if (r == 'A')
            return QJSValue(QString(QChar(value)));
        return QJSValue(value);
    }
    return QJSValue(value);
}
void DLObject::OnSend_Poke(int charNo, int value) {
    const int index = charNo > 0 ? charNo - 1 : m_host->m_onSendData.size() + charNo;
    if (index >= 0 && index < m_host->m_onSendData.size())
        m_host->m_onSendData[index] = char(qBound(0, value, 255));
}
int DLObject::OnReceive_GetSize() const {
    return m_host->m_onReceive.bytes.size();
}
QString DLObject::OnReceive_GetName() const {
    return m_host->m_onReceive.name;
}
int DLObject::OnReceive_GetIndex() const {
    return m_host->m_onReceive.index;
}
QString DLObject::OnReceive_GetData(const QString &representation, int start, int length) {
    return contextData(m_host->m_onReceive.bytes, representation, start, length);
}
QJSValue DLObject::OnReceive_Peek(int charNo, const QString &representation) {
    const int index = charNo > 0 ? charNo - 1 : m_host->m_onReceive.bytes.size() + charNo;
    if (index < 0 || index >= m_host->m_onReceive.bytes.size())
        return QJSValue(0);
    const uchar value = uchar(m_host->m_onReceive.bytes.at(index));
    if (!representation.isEmpty()) {
        const QChar r = representation.at(0).toUpper();
        if (r == 'H')
            return QJSValue(QStringLiteral("%1").arg(value, 2, 16, QChar('0')).toUpper());
        if (r == 'D')
            return QJSValue(QStringLiteral("%1").arg(value, 3, 10, QChar('0')));
        if (r == 'B')
            return QJSValue(QStringLiteral("%1").arg(value, 8, 2, QChar('0')));
        if (r == 'A')
            return QJSValue(QString(QChar(value)));
        return QJSValue(value);
    }
    return QJSValue(value);
}
int DLObject::OnReceive_GetChannel() const {
    return m_host->m_onReceive.channel;
}
double DLObject::OnReceive_GetDateTime() const {
    return double(m_host->m_onReceive.timestampMs / 1000);
}
int DLObject::OnReceive_GetMilliseconds() const {
    return int(m_host->m_onReceive.timestampMs % 1000);
}

// ---------------------------------------------------------------------------
// FileInput / FileOutput
// ---------------------------------------------------------------------------

FileInputObject::FileInputObject(const QString &path) {
    m_file.setFileName(path);
    m_open = m_file.open(QIODevice::ReadOnly | QIODevice::Text);
    m_stream.setDevice(&m_file);
}

QString FileInputObject::readChar() {
    if (atEnd())
        return QString();
    QChar c;
    m_stream >> c;
    return QString(c);
}

QString FileInputObject::readLine() {
    return m_open ? m_stream.readLine() : QString();
}

QString FileInputObject::readAll() {
    return m_open ? m_stream.readAll() : QString();
}

void FileInputObject::close() {
    m_file.close();
    m_open = false;
}

FileOutputObject::FileOutputObject(const QString &path, bool append) {
    m_file.setFileName(path);
    m_open = m_file.open(QIODevice::WriteOnly | (append ? QIODevice::Append
                                                        : QIODevice::Truncate));
    m_stream.setDevice(&m_file);
}

void FileOutputObject::write(const QString &text) {
    if (m_open)
        m_stream << text;
}

void FileOutputObject::writeLine(const QString &text) {
    if (m_open)
        m_stream << text << "\n";
}

void FileOutputObject::writeHex(const QString &hex) {
    if (!m_open)
        return;
    // Flush the text stream first so binary data keeps the right order.
    m_stream.flush();
    m_file.write(DataFormat::fromHex(hex));
}

void FileOutputObject::close() {
    if (m_open)
        m_stream.flush();
    m_file.close();
    m_open = false;
}


QJSValue DLObject::_openInput(const QString &path) {
    return m_host->jsEngine()->newQObject(new FileInputObject(path));
}

QJSValue DLObject::_openOutput(const QString &path, bool append) {
    return m_host->jsEngine()->newQObject(new FileOutputObject(path, append));
}
// ---------------------------------------------------------------------------
// Timing, conversions and misc
// ---------------------------------------------------------------------------

int DLObject::StartTime() {
    return int(QDateTime::currentMSecsSinceEpoch() - m_host->m_startMs);
}

double DLObject::TimeDiff() {
    return double(QDateTime::currentMSecsSinceEpoch() - m_host->m_startMs) / 1000.0;
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
    return QString::number(qBound(0, value, 0xFFFFFF), 16)
        .rightJustified(qBound(2, digits, 8), '0')
        .toUpper();
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

ScriptRunner::ScriptRunner(CommManager *comm, SequenceModel *sequences, QObject *bridge,
                           QObject *parent)
    : QObject(parent), m_comm(comm), m_sequences(sequences), m_bridge(bridge) {}

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
        entry.bytes = parsed.ok() ? parsed.bytes() : QByteArray();
        entry.wildcard = parsed.ok() ? parsed.wildcard() : QVector<bool>();
        snap.entries.append(entry);
    }
    return snap;
}

// Docklight's #include directive: splice the referenced file into the
// source at the directive's position. Relative paths resolve against the
// including file's directory, then the working directory; the .pts
// extension is optional. Cycles are cut off with an error comment.
namespace {
QString preprocessIncludes(QString code, const QDir &baseDir, int depth, QStringList *notes) {
    if (depth > 8) {
        notes->append(QStringLiteral("#include nesting too deep"));
        return code;
    }
    static const QRegularExpression includeRe(
        QStringLiteral("^[ \\t]*#include[ \\t]+(?:\"([^\"]+)\"|(\\S+))[ \\t]*$"),
        QRegularExpression::MultilineOption);
    QString out;
    qsizetype pos = 0;
    auto match = includeRe.globalMatch(code);
    // Rebuild line by line so includes splice in order.
    qsizetype lastEnd = 0;
    while (match.hasNext()) {
        const auto m = match.next();
        out += code.mid(lastEnd, m.capturedStart() - lastEnd);
        lastEnd = m.capturedEnd();
        QString path = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        if (!path.endsWith(QStringLiteral(".pts")) && !path.endsWith(QStringLiteral(".js"))
            && !QFileInfo(path).exists())
            path += QStringLiteral(".pts");
        QString resolved = path;
        if (QFileInfo(path).isRelative()) {
            const QString inBase = baseDir.filePath(path);
            if (QFile::exists(inBase))
                resolved = inBase;
        }
        QFile file(resolved);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            notes->append(QStringLiteral("#include file not found: %1").arg(path));
            out += QStringLiteral("// #include failed: %1").arg(path);
            continue;
        }
        const QString included = QString::fromUtf8(file.readAll());
        out += preprocessIncludes(included, QFileInfo(resolved).absolutePath(), depth + 1, notes);
    }
    out += code.mid(lastEnd);
    return out;
}
}  // namespace

void ScriptRunner::runFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit outputLine(tr("Cannot open script %1").arg(path), true);
        return;
    }
    const QString code = QString::fromUtf8(file.readAll());
    const QString dir = QFileInfo(path).absolutePath();
    QStringList notes;
    const QString processed = preprocessIncludes(code, dir, 0, &notes);
    for (const QString &note : notes)
        emit outputLine(note, true);
    run(processed);
}

void ScriptRunner::run(const QString &code) {
    if (m_running || code.trimmed().isEmpty())
        return;
    m_running = true;
    emit runningChanged();

    m_engine = new DLScriptEngine();
    m_engine->configure(m_comm, snapshot(), m_bridge);
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
    connect(m_engine, &DLScriptEngine::sendRequest, this, &ScriptRunner::sendRequest);
    connect(m_engine, &DLScriptEngine::dialogRequest, this, &ScriptRunner::dialogRequest);

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
