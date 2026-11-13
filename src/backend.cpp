#include "backend.h"

#include "checksum.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSaveFile>
#include <QPdfWriter>
#include <QTextDocument>
#include <QFontDatabase>
#include <QPageLayout>
#include <QStandardPaths>

#include "project.h"

Backend::Backend(QObject *parent)
    : QObject(parent),
      m_comm(new CommManager(this)),
      m_a(new ChannelSettings(this)),
      m_b(new ChannelSettings(this)),
      m_sequences(new SequenceModel(this)),
      m_log(new CommLog(this)),
      m_runner(new ScriptRunner(m_comm, m_sequences, this, this)),
      m_ports(new PortEnumerator(this)),
      m_picker(new PortalPicker(this)) {
    connect(m_comm, &CommManager::incoming, this, &Backend::handleIncoming);
    // DL.SendSequence requests from the script engine arrive queued; the
    // engine parks until scriptSendSequenceDone() flows back.
    connect(m_runner, &ScriptRunner::sendRequest, this, &Backend::scriptSendSequence,
            Qt::QueuedConnection);
    connect(m_runner, &ScriptRunner::dialogRequest, this, &Backend::scriptDialog,
            Qt::QueuedConnection);
    connect(m_comm, &CommManager::transmitted, this, &Backend::handleTransmitted);
    connect(m_comm, &CommManager::eventLogged, m_log, [this](const QString &text) {
        m_log->appendEvent(text);
        emit stateChanged();
    });
    connect(m_comm, &CommManager::runningChanged, this, [this] {
        if (m_comm->running()) {
            m_log->startClock();
            m_repeatClock.start();
            m_nextRepeatAt.clear();
            m_matchers[0].reset();
            m_matchers[1].reset();
            if (m_clockStampInterval > 0)
                m_clockTimer.start(m_clockStampInterval * 1000);
        } else {
            m_clockTimer.stop();
        }
        emit stateChanged();
    });
    connect(m_comm, &CommManager::countersChanged, this, &Backend::stateChanged);
    connect(m_log, &CommLog::displayChanged, this, &Backend::stateChanged);
    for (int ch = 0; ch < 2; ++ch) {
        connect(&m_matchers[ch], &SequenceMatcher::matched, this,
                [this, ch](int sequenceId, const QVector<SequenceMatcher::Token> &tokens) {
                    handleTrigger(sequenceId, tokens, ch);
                });
    }
    connect(m_sequences, &SequenceModel::sequenceChanged, this, &Backend::syncMatcher);
    syncMatcher();

    m_repeatTimer.setInterval(100);
    connect(&m_repeatTimer, &QTimer::timeout, this, &Backend::checkRepeats);

    // Docklight's "additional date/time stamp": a full stamp into the
    // communication window every N seconds while communication runs.
    connect(&m_clockTimer, &QTimer::timeout, this, [this] {
        logEvent(QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")));
    });

    // Everything below marks the project dirty, so an unsaved-changes prompt
    // can appear before quitting.
    wireDirtyTracking();
    connect(m_runner, &ScriptRunner::finished, this, &Backend::scriptFinished);
    connect(m_runner, &ScriptRunner::commentRequested, m_log,
            [this](const QString &text, bool pendingLine) {
        m_log->appendComment(text, pendingLine);
    });
    connect(m_runner, &ScriptRunner::clearRequested, m_log, &CommLog::clear);
    connect(m_runner, &ScriptRunner::saveLogRequested, this, &Backend::saveLogTo);
    connect(m_sequences, &SequenceModel::sendRequested, this, &Backend::sendSequence);
}

void Backend::wireDirtyTracking() {
    auto markDirty = [this] { setDirty(true); };
    connect(m_a, &ChannelSettings::changed, this, markDirty);
    connect(m_b, &ChannelSettings::changed, this, markDirty);
    connect(m_comm, &CommManager::modeChanged, this, markDirty);
    connect(m_sequences, &SequenceModel::sequenceChanged, this, markDirty);
    connect(this, &Backend::scriptChanged, this, markDirty);
}

void Backend::setDirty(bool dirty) {
    if (dirty == m_dirty)
        return;
    m_dirty = dirty;
    emit stateChanged();
}

void Backend::setProjectPath(const QString &path) {
    if (path == m_projectPath)
        return;
    m_projectPath = path;
    emit stateChanged();
}

QString Backend::commStateText() const {
    if (!m_comm->running())
        return tr("Stopped");
    return tr("Running · TX %1 · RX %2")
        .arg(QString::number(m_comm->txBytes()), QString::number(m_comm->rxBytes()));
}

bool Backend::runOnOpen() const {
    return m_runOnOpen;
}

void Backend::setClockStampInterval(int seconds) {
    if (seconds == m_clockStampInterval)
        return;
    m_clockStampInterval = qMax(0, seconds);
    if (m_clockStampInterval > 0 && m_comm->running())
        m_clockTimer.start(m_clockStampInterval * 1000);
    else
        m_clockTimer.stop();
    emit clockStampChanged();
}

void Backend::setKeyboardConsole(bool on) {
    if (on == m_keyboardConsole)
        return;
    m_keyboardConsole = on;
    emit keyboardConsoleChanged();
}

QString Backend::clipboardText() const {
    return QGuiApplication::clipboard()->text();
}

void Backend::setRunOnOpen(bool on) {
    if (on == m_runOnOpen)
        return;
    m_runOnOpen = on;
    setDirty(true);
    emit stateChanged();
}

void Backend::setScriptSource(const QString &source) {
    if (source == m_scriptSource)
        return;
    m_scriptSource = source;
    emit scriptChanged();
}

// ---------------------------------------------------------------------------
// Traffic
// ---------------------------------------------------------------------------

void Backend::setDialogPause(bool paused) {
    if (m_dialogPause == paused)
        return;
    m_dialogPause = paused;
    if (!paused) {
        // Process everything that arrived while the dialog was open.
        const auto held = m_heldIncoming;
        m_heldIncoming.clear();
        for (const auto &chunk : held)
            handleIncoming(chunk.first, chunk.second);
    }
    emit stateChanged();
}

void Backend::handleIncoming(int channel, const QByteArray &data) {
    if (m_dialogPause && QThread::currentThread() == thread()) {
        m_heldIncoming.append({channel, data});
        return;
    }
    // The matcher may fire a trigger for this very burst; remember that so
    // the log row gets the highlight mark.
    // Only the two main ports feed the receive matcher; side channels are
    // script-driven and must not participate in pattern matching.
    if (channel >= 0 && channel < 2)
        m_matchers[channel].feed(data, QDateTime::currentMSecsSinceEpoch());
    const bool highlighted = m_pendingHighlight;
    m_pendingHighlight = false;
    m_log->append(CommLog::Rx, channel, data, highlighted);
    m_logger.onRow(CommLog::Rx, channel, data, QString(),
                   QDateTime::currentMSecsSinceEpoch(), highlighted);
}

void Backend::handleTransmitted(int channel, const QByteArray &data) {
    m_log->append(CommLog::Tx, channel, data);
    m_logger.onRow(CommLog::Tx, channel, data, QString(),
                   QDateTime::currentMSecsSinceEpoch(), false);
}

void Backend::handleTrigger(int sequenceId, const QVector<SequenceMatcher::Token> &tokens,
                            int channel) {
    m_pendingHighlight = true;
    QByteArray received;
    for (const SequenceMatcher::Token &t : tokens)
        if (t.kind == SequenceMatcher::Token::Data)
            received.append(char(t.value));
    for (int i = 0; i < m_sequences->sequences().size(); ++i) {
        const Sequence &seq = m_sequences->sequences().at(i);
        if (seq.id != sequenceId || !seq.enabled)
            continue;
        // Docklight counts every detection since start/reset.
        m_receiveCounts[sequenceId]++;
        // The running script gets the match queued as a DL_OnReceive event.
        // Direct thread-safe handoff: the event is in the queue right now, so
        // a drain right after a blocking call cannot miss it.
        DLScriptEngine *engine = m_runner ? m_runner->engine() : nullptr;
        if (engine) {
            engine->queueReceiveEvent(seq.id, i, seq.name, received,
                                      channel + 1,
                                      QDateTime::currentMSecsSinceEpoch());
        }
        applyTrigger(&seq, received, channel);
        break;
    }
}

// Docklight's Receive Sequence comment macros: %_S %_L %_T %_C %_X %_I %_N
// %_A %_H %_D %_B, with optional (start, length) ranges on the data macros.
QString Backend::expandCommentMacros(const QString &text, const Sequence *seq,
                                     const QByteArray &received, int channel) {
    QString out;
    for (int i = 0; i < text.size();) {
        if (text.mid(i, 2) != QStringLiteral("%_")) {
            out.append(text.at(i));
            ++i;
            continue;
        }
        const QChar code = i + 2 < text.size() ? text.at(i + 2) : QChar();
        const int after = i + 3;
        int start = 0, length = -1;
        bool hasRange = false;
        if (after < text.size() && text.at(after) == QLatin1Char('(')) {
            const int close = text.indexOf(QLatin1Char(')'), after);
            if (close > after) {
                const QStringList parts =
                    text.mid(after + 1, close - after - 1).split(',');
                if (parts.size() == 2) {
                    start = parts.at(0).trimmed().toInt();
                    length = parts.at(1).trimmed().toInt();
                    hasRange = true;
                }
            }
        }
        const int bodyEnd = hasRange
                                ? text.indexOf(QLatin1Char(')'), after) + 1
                                : after;
        QString replacement;
        const auto dataText = [&](DataFormat::Format format) {
            QByteArray slice = received;
            if (hasRange) {
                // 1-based start, -1 = until last, -2 = second last, ...
                int begin = start > 0 ? start - 1 : received.size() + start;
                int end = length < 0 ? received.size() + length + 1 : begin + length;
                begin = qBound(0, begin, received.size());
                end = qBound(0, end, received.size());
                if (end < begin)
                    end = begin;
                slice = received.mid(begin, end - begin);
            }
            if (format == DataFormat::Format::Hex)
                return DataFormat::toHex(slice);
            return DataFormat::display(slice, format);
        };
        switch (code.unicode()) {
        case 'S': replacement = dataText(seq->format); break;   // data in sequence notation
        case 'L': replacement = QString::number(received.size()); break;
        case 'T': replacement = QLocale().toString(QDateTime::currentDateTime(),
                                                    QStringLiteral("HH:mm:ss.zzz"));
            break;
        case 'C': replacement = QString::number(channel + 1); break;
        case 'X': replacement = "RX"; break;   // receive matches are always RX
        case 'I': replacement = QString::number(indexOfSequence(seq->id)); break;
        case 'N': replacement = seq->name; break;
        case 'A': replacement = dataText(DataFormat::Format::Text); break;
        case 'H': replacement = dataText(DataFormat::Format::Hex); break;
        case 'D': replacement = dataText(DataFormat::Format::Dec); break;
        default: break;
        case 'B': {
            QString bin;
            const QStringList hexBytes = dataText(DataFormat::Format::Hex)
                                             .split(' ', Qt::SkipEmptyParts);
            for (const QString &hexByte : hexBytes) {
                if (!bin.isEmpty())
                    bin += QLatin1Char(' ');
                bin += QString("%1").arg(hexByte.toInt(nullptr, 16), 8, 2, QChar('0'));
            }
            replacement = bin;
            break;
        }
        }
        out.append(replacement);
        i = qMax(bodyEnd, i + 3);
    }
    return out;
}

int Backend::indexOfSequence(int id) const {
    const auto &list = m_sequences->sequences();
    for (int i = 0; i < list.size(); ++i)
        if (list.at(i).id == id)
            return i;
    return -1;
}

void Backend::applyTrigger(const Sequence *receiveSequence, const QByteArray &matched,
                           int matchChannel) {
    // Action: Answer — transmit one send sequence (wildcards not allowed).
    if (receiveSequence->replyToId >= 0) {
        for (const Sequence &candidate : m_sequences->sequences()) {
            if (candidate.id != receiveSequence->replyToId || !candidate.isSend)
                continue;
            const auto parsed = DataFormat::parse(candidate.data, candidate.format);
            const bool hasWildcard = std::any_of(parsed.elements.begin(), parsed.elements.end(),
                                                 [](const ParsedBytes::Element &e) {
                                                     return e.kind == ParsedBytes::Element::WildOne
                                                            || e.kind == ParsedBytes::Element::WildZeroOrOne;
                                                 });
            if (!parsed.ok() || hasWildcard) {
                m_log->appendEvent(tr("Trigger %1: reply sequence has invalid data")
                                       .arg(receiveSequence->name));
                return;
            }
            if (m_comm->send(parsed.bytes()))
                m_log->appendEvent(tr("Trigger %1 → sent %2").arg(receiveSequence->name, candidate.name));
            else
                m_log->appendEvent(tr("Trigger %1: send failed").arg(receiveSequence->name));
            break;
        }
    }
    // Action: Comment — macro-expanded text into the log.
    if (!receiveSequence->comment.isEmpty()) {
        const QString expanded = expandCommentMacros(receiveSequence->comment, receiveSequence,
                                                     matched, matchChannel);
        logComment(expanded);
        m_receiveComments.append(expanded);
    }
    // Action: Trigger — snapshot mark (highlighted rows in the log).
    if (receiveSequence->trigger)
        m_pendingHighlight = true;
    // Action: Stop — end the test run.
    if (receiveSequence->stopRun) {
        m_log->appendEvent(tr("Stop sequence %1 matched — stopping communication")
                               .arg(receiveSequence->name));
        m_comm->stop();
    }
}

// Rebuild the receive-trigger patterns from the sequence table. Called at
// startup and whenever the table changes.
void Backend::syncMatcher() {
    QVector<SequenceMatcher::Pattern> patterns;
    for (const Sequence &seq : m_sequences->sequences()) {
        if (seq.isSend || !seq.enabled)
            continue;
        const auto parsed = DataFormat::parse(seq.data, seq.format);
        if (!parsed.ok())
            continue;
        bool hasData = false;
        for (const auto &e : parsed.elements)
            if (e.kind == ParsedBytes::Element::Data
                || e.kind == ParsedBytes::Element::WildOne
                || e.kind == ParsedBytes::Element::WildZeroOrOne
                || e.kind == ParsedBytes::Element::Bitwise
                || e.kind == ParsedBytes::Element::Break
                || e.kind == ParsedBytes::Element::Handshake)
                hasData = true;
        if (!hasData && seq.checksumSpec.isEmpty())
            continue;
        SequenceMatcher::Pattern pattern;
        pattern.sequenceId = seq.id;
        pattern.elements = parsed.elements;
        if (!seq.checksumSpec.isEmpty()) {
            pattern.hasChecksum = true;
            pattern.checksumSpec = seq.checksumSpec;
            pattern.checksumMode = seq.checksumMode;
        }
        patterns.append(pattern);
    }
    m_matchers[0].setPatterns(patterns);
    m_matchers[1].setPatterns(patterns);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void Backend::toggleComm() {
    if (m_comm->running()) {
        m_comm->stop();
        return;
    }
    if (m_comm->start(m_a->config(), m_b->config())) {
        m_repeatTimer.start();
        m_repeatClock.start();
        m_nextRepeatAt.clear();
        if (m_clockStampInterval > 0)
            m_clockTimer.start(m_clockStampInterval * 1000);
    } else {
        // start() can fail before the links exist (empty device).
        CommLink *linkA = m_comm->link(0);
        emit toast(linkA ? linkA->lastError() : tr("Communication could not be started"));
    }
}

bool Backend::sendText(const QString &text, int format) {
    const auto parsed = DataFormat::parse(text, DataFormat::Format(format));
    if (!parsed.ok()) {
        emit toast(parsed.error);
        return false;
    }
    if (!m_comm->send(parsed.bytes())) {
        if (!m_comm->running())
            emit toast(tr("Communication is not running"));
        return false;
    }
    return true;
}


// Docklight calls the DL_OnSend() script procedure before every Send
// Sequence transmission (manual sends included). The handler can read the
// data, replace it with DL.OnSend_SetData, or suppress it with an empty
// string. Returns false when the transmission is suppressed.
bool Backend::applyOnSend(const Sequence &seq, QVector<ParsedBytes::Element> &elements, int rowIndex) {
    if (!m_runner || !m_runner->running() || !m_runner->engine())
        return true;
    QByteArray bytes;
    for (const auto &e : elements)
        if (e.kind == ParsedBytes::Element::Data
            || e.kind == ParsedBytes::Element::WildOne
            || e.kind == ParsedBytes::Element::WildZeroOrOne)
            bytes.append(char(e.value));
    QByteArray modified;
    QMetaObject::invokeMethod(m_runner->engine(), "processOnSend",
                              Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(QByteArray, modified),
                              Q_ARG(QString, seq.name), Q_ARG(int, rowIndex),
                              Q_ARG(QByteArray, bytes));
    if (modified.isEmpty())
        return false;   // OnSend_SetData "" suppresses the transmission
    if (modified == bytes)
        return true;
    const auto parsed = DataFormat::parse(DataFormat::toHex(modified),
                                          DataFormat::Format::Hex);
    if (parsed.ok())
        elements = parsed.elements;
    return true;
}

void Backend::sendSequence(int row) {
    const Sequence *seq = m_sequences->sequenceAt(row);
    if (!seq || !seq->isSend)
        return;
    // A sequence with wildcard parameters asks for values first, exactly
    // like Docklight's Send Sequence Parameter dialog.
    const auto parsed = DataFormat::parse(seq->data, seq->format);
    if (!parsed.ok()) {
        emit toast(tr("%1: %2").arg(seq->name, parsed.error));
        return;
    }
    QStringList minLengths;
    int run = 0;
    for (const auto &e : parsed.elements) {
        if (e.kind == ParsedBytes::Element::WildOne
            || e.kind == ParsedBytes::Element::WildZeroOrOne)
            ++run;
        else if (run) {
            minLengths.append(QString::number(run));
            run = 0;
        }
    }
    if (run)
        minLengths.append(QString::number(run));
    if (!minLengths.isEmpty()) {
        // Consecutive wildcard positions form one parameter each.
        emit parameterRequest(row, minLengths);
        return;
    }
    // The no-parameter path goes through the same builder so checksum areas
    // are applied, like Docklight computing the checksum on every send.
    QVector<ParsedBytes::Element> elements;
    QString error;
    if (!buildSendElements(*seq, QStringList(), DataFormat::Format::Text, &elements, &error)) {
        emit toast(tr("%1: %2").arg(seq->name, error));
        return;
    }
    if (applyOnSend(*seq, elements, row)) {
        transmitElements(elements);
        armRepeat(seq);
    } else {
        emit scriptSendSequenceDone();
    }
}

void Backend::armRepeat(const Sequence *seq, const QStringList &parameters) {
    if (!seq || seq->repeatIntervalMs <= 0)
        return;
    if (m_nextRepeatAt.contains(seq->id)) {
        m_nextRepeatAt.remove(seq->id);
        m_repeatParameters.remove(seq->id);
        m_log->appendEvent(tr("Repeat stopped: %1").arg(seq->name));
    } else {
        m_repeatParameters.insert(seq->id, parameters);
        m_nextRepeatAt.insert(seq->id, m_repeatClock.elapsed() + seq->repeatIntervalMs);
        m_log->appendEvent(tr("Repeat every %1 ms: %2").arg(seq->repeatIntervalMs).arg(seq->name));
    }
}

QStringList Backend::sequenceWildcardAreas(int row) const {
    const Sequence *seq = m_sequences->sequenceAt(row);
    if (!seq || !seq->isSend)
        return {};
    const auto parsed = DataFormat::parse(seq->data, seq->format);
    if (!parsed.ok())
        return {};
    QStringList minLengths;
    int run = 0;
    for (const auto &e : parsed.elements) {
        if (e.kind == ParsedBytes::Element::WildOne
            || e.kind == ParsedBytes::Element::WildZeroOrOne)
            ++run;
        else if (run) {
            minLengths.append(QString::number(run));
            run = 0;
        }
    }
    if (run)
        minLengths.append(QString::number(run));
    return minLengths;
}

void Backend::sendSequenceWithParams(int row, const QStringList &parameters) {
    const Sequence *seq = m_sequences->sequenceAt(row);
    if (!seq || !seq->isSend)
        return;
    QVector<ParsedBytes::Element> elements;
    QString error;
    if (!buildSendElements(*seq, parameters, DataFormat::Format::Text, &elements, &error)) {
        emit toast(tr("%1: %2").arg(seq->name, error));
        return;
    }
    if (applyOnSend(*seq, elements, row)) {
        transmitElements(elements);
        armRepeat(seq, parameters);
    } else {
        emit scriptSendSequenceDone();
    }
}

void Backend::sendByHotkey(int keyNumber) {
    const int row = m_sequences->findSendByHotkey(keyNumber);
    if (row >= 0)
        sendSequence(row);
}

void Backend::checkRepeats() {
    if (m_dialogPause)
        return;
    if (!m_comm->running()) {
        if (!m_nextRepeatAt.isEmpty())
            m_nextRepeatAt.clear();
        m_repeatTimer.stop();
        m_clockTimer.stop();
        return;
    }
    const qint64 now = m_repeatClock.elapsed();
    for (auto it = m_nextRepeatAt.begin(); it != m_nextRepeatAt.end();) {
        if (now < it.value()) {
            ++it;
            continue;
        }
        const int seqId = it.key();
        it = m_nextRepeatAt.erase(it);
        for (const Sequence &seq : m_sequences->sequences()) {
            if (seq.id != seqId || !seq.enabled)
                continue;
            // Periodic sends go through the same builder as manual sends so
            // wildcard parameters and checksum areas are recomputed each time,
            // exactly like Docklight's periodic send.
            QVector<ParsedBytes::Element> elements;
            QString error;
            if (!buildSendElements(seq, m_repeatParameters.value(seqId),
                                   DataFormat::Format::Text, &elements, &error)) {
                m_log->appendEvent(tr("Repeat send failed: %1").arg(seq.name));
                break;
            }
            transmitElements(elements);
            m_nextRepeatAt.insert(seqId, now + seq.repeatIntervalMs);
            break;
        }
    }
}

// Fills wildcard runs with the given parameters and applies the checksum
// area, producing the final element stream to transmit.
bool Backend::buildSendElements(const Sequence &seq, const QStringList &parameters,
                                DataFormat::Format paramFormat,
                                QVector<ParsedBytes::Element> *out, QString *error) {
    const auto parsed = DataFormat::parse(seq.data, seq.format);
    if (!parsed.ok()) {
        if (error)
            *error = parsed.error;
        return false;
    }
    QVector<ParsedBytes::Element> elements = parsed.elements;

    // Docklight: consecutive wildcards form one parameter. ASCII parameters
    // are space-separated; HEX/DEC/BIN use commas. Shortfalls pad with
    // spaces (ASCII) or zero bytes, surplus is truncated.
    QStringList params = parameters;
    int run = 0, paramIndex = 0;
    QVector<std::pair<int, int>> runs;   // first element index, length
    for (int i = 0; i < elements.size(); ++i) {
        const auto kind = elements.at(i).kind;
        if (kind == ParsedBytes::Element::WildOne
            || kind == ParsedBytes::Element::WildZeroOrOne) {
            if (run == 0)
                runs.append({i, 0});
            ++run;
            runs.last().second = run;
        } else if (run) {
            run = 0;
        }
    }
    const auto fillByte = [&](uchar &value, const QString &text) {
        if (paramFormat == DataFormat::Format::Text) {
            const QByteArray encoded = text.left(1).toUtf8();
            value = encoded.isEmpty() ? uchar(0x20) : uchar(encoded.at(0));
            return;
        }
        bool ok = false;
        const int number = paramFormat == DataFormat::Format::Hex
                               ? text.toInt(&ok, 16)
                               : paramFormat == DataFormat::Format::Dec
                                     ? text.toInt(&ok, 10)
                                     : text.toInt(&ok, 2);
        value = uchar(ok ? qBound(0, number, 255) : 0);
    };
    for (const auto &runInfo : runs) {
        // Docklight: one parameter fills the whole wildcard area with its
        // characters (ASCII), truncated or padded with blanks. Hex/Dec/Bin
        // parameters list one byte value per wildcard, space-separated
        // ("20 31 20"); surplus values are ignored, shortfalls pad with
        // ASCII code 0.
        const QString param = paramIndex < params.size() ? params.at(paramIndex) : QString();
        ++paramIndex;
        QStringList pieces;
        if (paramFormat == DataFormat::Format::Text) {
            for (int k = 0; k < runInfo.second; ++k) {
                uchar value = 0x20;   // ASCII blank padding
                if (k < param.size()) {
                    const QByteArray encoded = QString(param.at(k)).toUtf8();
                    value = encoded.isEmpty() ? 0x20 : uchar(encoded.at(0));
                }
                elements[runInfo.first + k].kind = ParsedBytes::Element::Data;
                elements[runInfo.first + k].value = value;
            }
            continue;
        }
        pieces = param.split(' ', Qt::SkipEmptyParts);
        for (int k = 0; k < runInfo.second; ++k) {
            const QString piece = k < pieces.size() ? pieces.at(k) : QString();
            uchar value = 0;
            fillByte(value, piece);
            elements[runInfo.first + k].kind = ParsedBytes::Element::Data;
            elements[runInfo.first + k].value = value;
        }
    }

    // Checksum area: operate on the byte image of the data positions.
    if (!seq.checksumSpec.isEmpty()) {
        const Checksum::Area area = Checksum::parseArea(seq.checksumSpec);
        if (!area.ok) {
            if (error)
                *error = area.error;
            return false;
        }
        QByteArray image;
        QVector<int> byteToElement;
        for (int i = 0; i < elements.size(); ++i) {
            if (elements.at(i).kind == ParsedBytes::Element::Data) {
                image.append(char(elements.at(i).value));
                byteToElement.append(i);
            }
        }
        if (!Checksum::applyToSequence(area, image, error))
            return false;
        for (int b = 0; b < image.size() && b < byteToElement.size(); ++b)
            elements[byteToElement.at(b)].value = uchar(image.at(b));
    }

    *out = elements;
    return true;
}

// Transmits a prepared element stream: byte runs go out as bursts, delays
// park the send in the event loop, break and '!' elements drive the line.
void Backend::transmitElements(const QVector<ParsedBytes::Element> &elements) {
    QByteArray burst;
    int paritySetting = -1;   // '!' parity changes, restored afterwards
    auto flushBurst = [&]() {
        if (!burst.isEmpty()) {
            if (!m_comm->send(burst) && !m_comm->running())
                emit toast(tr("Communication is not running"));
            burst.clear();
        }
    };
    for (const auto &e : elements) {
        switch (e.kind) {
        case ParsedBytes::Element::Data:
            burst.append(char(e.value));
            break;
        case ParsedBytes::Element::WildOne:
        case ParsedBytes::Element::WildZeroOrOne:
            burst.append(char(0x20));   // unfilled wildcard (defensive)
            break;
        case ParsedBytes::Element::Bitwise:
            break;   // receive-only
        case ParsedBytes::Element::Delay: {
            flushBurst();
            QEventLoop loop;
            QTimer::singleShot(int(e.value) * 10, &loop, &QEventLoop::quit);
            loop.exec();
            break;
        }
        case ParsedBytes::Element::Break:
            flushBurst();
            m_comm->sendBreak(200);
            break;
        case ParsedBytes::Element::Handshake: {
            flushBurst();
            const uchar v = e.value;
            if (v <= 3) {
                m_comm->setRTS(0, v & 1);
                m_comm->setDTR(0, v & 2);
            } else if (v >= 16 && v <= 80) {
                // 16 none, 32 odd, 48 even, 64 mark, 80 space
                const int parity = v == 16 ? 0 : v == 32 ? 2 : v == 48 ? 1
                                      : v == 64 ? 3 : 4;
                if (paritySetting < 0)
                    paritySetting = m_comm->link(0)->parity();
                m_comm->link(0)->setParity(parity);
            }
            break;
        }
        }
    }
    flushBurst();
    if (paritySetting >= 0)
        m_comm->link(0)->setParity(paritySetting);
}

// ---- script bridge --------------------------------------------------------

int Backend::scriptReceiveCount(int id) {
    return m_receiveCounts.value(id, 0);
}

void Backend::scriptResetCounters(int id) {
    if (id < 0)
        m_receiveCounts.clear();
    else
        m_receiveCounts.remove(id);
    if (id < 0) {
        m_matchers[0].reset();
        m_matchers[1].reset();
    }
}

QString Backend::scriptDrainReceiveComments() {
    const QStringList drained = m_receiveComments;
    m_receiveComments.clear();
    // Docklight stores at most 10000 receive comments.
    if (m_receiveComments.size() > 10000)
        m_receiveComments.remove(0, m_receiveComments.size() - 10000);
    return drained.join(QLatin1Char('\n'));
}

QString Backend::scriptChannelSettings(int channel) {
    // Docklight numbers channels 1 and 2; internally they are A and B.
    const ChannelSettings *settings = channel == 2 ? m_b : m_a;
    const SerialConfig &config = settings->config();
    // Network channels carry their whole configuration in the device string.
    if (CommLink::isNetworkDevice(config.device))
        return config.device;
    static const char *parityNames[] = {"NONE", "EVEN", "ODD", "MARK", "SPACE"};
    static const char *flowNames[] = {"OFF", "RTSCTS", "XONXOFF", "RTSSEND"};
    return QStringLiteral("%1:%2,%3,%4,%5,%6,%7")
        .arg(config.device,
             QString::number(config.baud),
             parityNames[qBound(0, config.parity, 4)],
             QString::number(config.dataBits),
             QString::number(config.stopBits),
             flowNames[qBound(0, config.flow, 3)],
             QString::number(config.parityErrorChar));
}

bool Backend::scriptSetChannelSettings(const QString &settings, int channel) {
    ChannelSettings *target = channel == 2 ? m_b : m_a;
    QString text = settings.trimmed();
    if (text == QLatin1String(">")) {
        // Find the next available serial port.
        const QStringList ports = m_ports->ports();
        const QString current = target->config().device;
        int index = ports.indexOf(current);
        for (int step = 1; step <= ports.size(); ++step) {
            const QString candidate = ports.at((qMax(0, index) + step) % ports.size());
            SerialLink probe;
            SerialConfig config = target->config();
            config.device = candidate;
            if (probe.open(config)) {
                probe.close();
                target->setDevice(candidate);
                return true;
            }
        }
        return false;
    }
    // Forms: "DEVICE", "DEVICE:baud,P,D,S[,FLOW[,PEC]]", "baud,P,D,S..."
    // Network device strings (TCP:host:port, SERVER:port, UNIXSERVER:/path,
    // ...) carry colons of their own and must never be split apart.
    if (CommLink::isNetworkDevice(text)) {
        target->setDevice(text.trimmed());
        if (m_comm->running())
            return m_comm->applySettings(m_a->config(), m_b->config());
        return true;
    }
    // Docklight: a bare "host:port" makes the channel a TCP client.
    static const QRegularExpression bareHost(
        QStringLiteral("^([^,:\\s]+):(\\d+)$"));
    if (bareHost.match(text).hasMatch()) {
        target->setDevice(CommLink::normalizeDevice(text));
        if (m_comm->running())
            return m_comm->applySettings(m_a->config(), m_b->config());
        return true;
    }
    const int colon = text.indexOf(QLatin1Char(':'));
    QString devicePart, tail;
    if (colon > 0 && !text.left(colon).contains(',')) {
        devicePart = text.left(colon).trimmed();
        tail = text.mid(colon + 1).trimmed();
    } else if (text.contains(',')) {
        tail = text;
    } else {
        devicePart = text;
    }
    if (!devicePart.isEmpty())
        target->setDevice(CommLink::normalizeDevice(devicePart));
    if (!tail.isEmpty()) {
        const QStringList parts = tail.split(',');
        bool ok = false;
        const int baud = parts.value(0).trimmed().toInt(&ok);
        if (ok && baud > 0)
            target->setBaud(baud);
        const QString parity = parts.value(1).trimmed().toUpper();
        int parityValue = -1;
        if (parity == "NONE") parityValue = 0;
        else if (parity == "EVEN") parityValue = 1;
        else if (parity == "ODD") parityValue = 2;
        else if (parity == "MARK") parityValue = 3;
        else if (parity == "SPACE") parityValue = 4;
        if (parityValue >= 0)
            target->setParity(parityValue);
        const int dataBits = parts.value(2).trimmed().toInt();
        if (dataBits >= 5 && dataBits <= 8)
            target->setDataBits(dataBits);
        const int stopBits = parts.value(3).trimmed().toInt();
        if (stopBits == 1 || stopBits == 2)
            target->setStopBits(stopBits);
        const QString flow = parts.value(4).trimmed().toUpper();
        int flowValue = -1;
        if (flow == "OFF") flowValue = 0;
        else if (flow == "RTSCTS") flowValue = 1;
        else if (flow == "XONXOFF") flowValue = 2;
        if (flowValue >= 0)
            target->setFlow(flowValue);
        if (parts.size() >= 6) {
            bool pecOk = false;
            const int pec = parts.value(5).trimmed().toInt(&pecOk);
            if (pecOk && pec >= 0 && pec <= 255)
                target->setParityErrorChar(pec);
        }
    }
    // Network device strings carry their own configuration; accept them as
    // they are. A serial device is probed before the settings stick.
    if (CommLink::isNetworkDevice(target->config().device)) {
        if (m_comm->running())
            return m_comm->applySettings(m_a->config(), m_b->config());
        return true;
    }
    // A running link picks the new settings up on restart; test the port.
    SerialLink probe;
    if (probe.open(target->config())) {
        probe.close();
        if (m_comm->running())
            return m_comm->applySettings(m_a->config(), m_b->config());
        return true;
    }
    // Docklight reports a channel error for a non-existing COM port while
    // communication is running: apply the settings anyway so the channel
    // enters the error state and GetChannelStatus returns 3.
    if (m_comm->running()) {
        m_comm->applySettings(m_a->config(), m_b->config());
        return false;
    }
    return false;
}

int Backend::scriptChannelStatus(int channel) {
    if (!m_comm->running())
        return 0;
    // Side channels 3..10 live in their own link table.
    if (channel >= 3 && channel <= 10) {
        const CommLink *side = m_comm->sideLink(channel);
        if (!side)
            return 0;
        const QString dev = side->device();
        if ((dev.startsWith("SERVER:") || dev.startsWith("TCPCLIENT:")
             || dev.startsWith("UNIXSERVER:") || dev.startsWith("UNIXCLIENT:"))
            && !side->isOpen())
            return 2;
        return side->isOpen() ? 1 : 3;
    }
    const CommLink *link = m_comm->link(channel == 2 ? 1 : 0);
    if (!link)
        return 3;
    // A listening server or an unconnected client reports "waiting", like
    // Docklight's status 2.
    const QString dev = link->device();
    if ((dev.startsWith("SERVER:") || dev.startsWith("TCPCLIENT:")
         || dev.startsWith("UNIXSERVER:") || dev.startsWith("UNIXCLIENT:"))
        && !link->isOpen())
        return 2;
    if (!link->isOpen())
        return 3;
    return 1;
}

bool Backend::scriptOpenProject(const QString &path) {
    const QString trimmed = path.trimmed();
    QString error;
    if (trimmed.isEmpty())
        return false;
    // Reading is quick and safe enough from the bridge call.
    QFile file(trimmed.endsWith(".llproj", Qt::CaseInsensitive) ? trimmed : trimmed + ".llproj");
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    error = Project::load(doc.object(), m_comm, m_a, m_b, m_sequences,
                          &m_scriptSource, m_log);
    if (!error.isEmpty())
        return false;
    m_projectPath = file.fileName();
    setDirty(false);
    emit scriptChanged();
    syncMatcher();
    m_receiveCounts.clear();
    return true;
}

void Backend::scriptSetContentsFilter(int filter) {
    emit contentsFilterChanged(qBound(0, filter, 3));
}

void Backend::scriptSetWindowLayout(const QString &layout) {
    emit windowLayoutChanged(layout);
}

QString Backend::scriptCommWindowData(const QString &representation) {
    QByteArray all;
    for (const auto &row : m_log->rows())
        all.append(row.data);
    // Docklight caps the GetCommWindowData buffer at 128000 characters and
    // drops the oldest data.
    if (all.size() > 128000)
        all = all.right(128000);
    if (representation.compare(QLatin1String("H"), Qt::CaseInsensitive) == 0)
        return DataFormat::toHex(all);
    if (representation.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0)
        return DataFormat::display(all, DataFormat::Format::Dec);
    if (representation.compare(QLatin1String("B"), Qt::CaseInsensitive) == 0) {
        QString bin;
        for (uchar byte : all) {
            if (!bin.isEmpty())
                bin += QLatin1Char(' ');
            bin += QString("%1").arg(byte, 8, 2, QChar('0'));
        }
        return bin;
    }
    return DataFormat::display(all, DataFormat::Format::Text);
}

bool Backend::scriptStartLogging(const QString &path, bool appendData,
                                 const QString &representations, int format,
                                 bool noHeaders) {
    return m_logger.start(path, appendData, representations, format, noHeaders);
}

void Backend::scriptStopLogging() {
    m_logger.stop();
}

void Backend::scriptSetUserOutput(const QString &text) {
    emit userOutputChanged(text);
}

// DL.SendSequence: the worker posts the request, waits while parked, and
// this slot runs on the main thread where the ports live.
void Backend::scriptSendSequence(int row, const QString &customData,
                                 const QString &representation,
                                 const QStringList &parameters) {
    QVector<ParsedBytes::Element> elements;
    if (row >= 0) {
        const Sequence *seq = m_sequences->sequenceAt(row);
        if (!seq || !seq->isSend) {
            emit scriptSendSequenceDone();
            return;
        }
        DataFormat::Format paramFormat = DataFormat::Format::Text;
        if (representation.compare(QLatin1String("H"), Qt::CaseInsensitive) == 0)
            paramFormat = DataFormat::Format::Hex;
        else if (representation.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0)
            paramFormat = DataFormat::Format::Dec;
        else if (representation.compare(QLatin1String("B"), Qt::CaseInsensitive) == 0)
            paramFormat = DataFormat::Format::Bin;
        QString error;
        if (!buildSendElements(*seq, parameters, paramFormat, &elements, &error)) {
            m_log->appendEvent(tr("SendSequence %1: %2").arg(seq->name, error));
            emit scriptSendSequenceDone();
            return;
        }
        // Let the parked script manipulate the data via DL_OnSend().
        if (m_runner->running() && m_runner->engine()) {
            QByteArray bytes;
            for (const auto &e : elements)
                if (e.kind == ParsedBytes::Element::Data
                    || e.kind == ParsedBytes::Element::WildOne
                    || e.kind == ParsedBytes::Element::WildZeroOrOne)
                    bytes.append(char(e.value));
            QByteArray modified;
            QString name = seq->name;
            const int index = row;
            QMetaObject::invokeMethod(m_runner->engine(), "processOnSend",
                                      Qt::BlockingQueuedConnection,
                                      Q_RETURN_ARG(QByteArray, modified),
                                      Q_ARG(QString, name), Q_ARG(int, index),
                                      Q_ARG(QByteArray, bytes));
            if (modified.isEmpty()) {
                // OnSend_SetData "" suppresses the transmission entirely.
                emit scriptSendSequenceDone();
                return;
            }
            if (modified != bytes) {
                const auto parsed = DataFormat::parse(
                    DataFormat::toHex(modified), DataFormat::Format::Hex);
                elements = parsed.ok() ? parsed.elements : elements;
            }
        }
    } else if (!customData.isEmpty()) {
        DataFormat::Format format = DataFormat::Format::Text;
        if (representation.compare(QLatin1String("H"), Qt::CaseInsensitive) == 0)
            format = DataFormat::Format::Hex;
        else if (representation.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0)
            format = DataFormat::Format::Dec;
        const auto parsed = DataFormat::parse(customData, format);
        if (!parsed.ok()) {
            emit scriptSendSequenceDone();
            return;
        }
        elements = parsed.elements;
    }
    if (!m_comm->running())
        m_comm->restart();
    transmitElements(elements);
    emit scriptSendSequenceDone();
}

void Backend::scriptSendSequenceDone() {
    // Emitted after each queued DL.SendSequence went out. The signal body
    // wakes the parked worker directly, mirroring Docklight's synchronous
    // hand-off; the parked waitUntil() picks it up in its next 50 ms slice.
    DLScriptEngine *engine = m_runner ? m_runner->engine() : nullptr;
    if (engine)
        QMetaObject::invokeMethod(engine, "onSendRequestDone", Qt::QueuedConnection);
}

// Docklight's F2/F3: continuous logging into the _asc/_hex/_dec/_bin files
// next to the project. Base name: <project>_<date>_<time>.
void Backend::startFileLogging() {
    if (m_logger.active())
        return;
    const QString base = projectPath().isEmpty()
                             ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                             : projectPath();
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QFileInfo info(base);
    const QString name = info.completeBaseName().isEmpty()
                             ? QStringLiteral("lucidlight") : info.completeBaseName();
    if (m_logger.start(info.absolutePath() + "/" + name + "_" + stamp, false, "AHDB", 0, false))
        emit stateChanged();
    else
        emit toast(tr("Cannot start logging"));
}

void Backend::stopFileLogging() {
    if (!m_logger.active())
        return;
    m_logger.stop();
    emit stateChanged();
}

bool Backend::fileLogging() const {
    return m_logger.active();
}

void Backend::saveScriptTo(const QString &path) {
    if (path.isEmpty())
        return;
    const QString finalPath = path.endsWith(".js", Qt::CaseInsensitive) ? path : path + ".js";
    QFile file(finalPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit toast(tr("Cannot write %1").arg(finalPath));
        return;
    }
    file.write(m_scriptSource.toUtf8());
    emit toast(tr("Script saved to %1").arg(finalPath));
}

void Backend::runScript(const QString &source) {
    if (source.trimmed().isEmpty())
        return;
    m_runner->run(source);
}

void Backend::runScriptFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit toast(tr("Cannot open script %1").arg(path));
        return;
    }
    emit toast(tr("Startup script: %1").arg(path));
    m_runner->runFile(path);
}

QVariant Backend::scriptInvoke(const QString &name, const QVariantList &args) {
    auto arg = [&args](int i) { return args.value(i); };
    if (name == "scriptStartCommunication") {
        if (m_comm->running())
            return true;
        toggleComm();
        return m_comm->running();
    }
    if (name == "scriptChannelStatus")
        return scriptChannelStatus(arg(0).toInt());
    if (name == "scriptChannelSettings")
        return scriptChannelSettings(arg(0).toInt());
    if (name == "scriptSetChannelSettings")
        return scriptSetChannelSettings(arg(0).toString(), arg(1).toInt());
    if (name == "scriptOpenProject")
        return scriptOpenProject(arg(0).toString());
    if (name == "scriptLoadOptions")
        return scriptLoadOptions(arg(0).toString());
    if (name == "scriptSaveOptions")
        return scriptSaveOptions(arg(0).toString());
    if (name == "scriptStartLogging")
        return scriptStartLogging(arg(0).toString(), arg(1).toBool(), arg(2).toString(),
                                  arg(3).toInt(), arg(4).toBool());
    if (name == "scriptStopLogging") {
        scriptStopLogging();
        return {};
    }
    if (name == "scriptSetContentsFilter") {
        scriptSetContentsFilter(arg(0).toInt());
        return {};
    }
    if (name == "scriptSetWindowLayout") {
        scriptSetWindowLayout(arg(0).toString());
        return {};
    }
    if (name == "scriptCommWindowData")
        return scriptCommWindowData(arg(0).toString());
    if (name == "scriptSetUserOutput") {
        scriptSetUserOutput(arg(0).toString());
        return {};
    }
    if (name == "scriptPortList")
        return scriptPortList();
    if (name == "scriptReceiveCount")
        return scriptReceiveCount(arg(0).toInt());
    if (name == "scriptResetCounters") {
        scriptResetCounters(arg(0).toInt());
        return {};
    }
    if (name == "scriptDrainReceiveComments")
        return scriptDrainReceiveComments();
    qWarning() << "scriptInvoke: unknown method" << name;
    return {};
}

QString Backend::scriptPortList() {
    return m_ports->ports().join(QLatin1Char('\n'));
}

bool Backend::scriptLoadOptions(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject options = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject display = options.value("display").toObject();
    m_log->setDisplayFormat(Project::displayFormatFromName(
        display.value("format").toString(QStringLiteral("text"))));
    m_log->setShowTimestamps(display.value("timestamps").toBool(true));
    m_log->setShowDelta(display.value("delta").toBool(false));
    return true;
}

bool Backend::scriptSaveOptions(const QString &path) {
    QJsonObject options;
    QJsonObject display;
    display["format"] = Project::displayFormatName(m_log->displayFormat());
    display["timestamps"] = m_log->showTimestamps();
    display["delta"] = m_log->showDelta();
    options["display"] = display;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(options).toJson());
    return true;
}

// Script dialogs: hand the request to the UI (or the headless runner, which
// answers from the environment) and wake the worker when the answer lands.
void Backend::scriptDialog(const QString &prompt, int buttons, const QString &title,
                           bool input, const QString &defaultText) {
    // Headless auto-answer: LUCID_MSGBOX gives the button code, LUCID_INPUTBOX
    // the text; without them a message box answers OK (or Yes for yes/no
    // boxes) and an input box returns the default text.
    const QByteArray boxAnswer = qgetenv(input ? "LUCID_INPUTBOX" : "LUCID_MSGBOX");
    if (qEnvironmentVariableIsSet("LUCID_HEADLESS")) {
        DLScriptEngine *engine = m_runner ? m_runner->engine() : nullptr;
        if (engine) {
            int answer;
            if (input) {
                answer = 0;
            } else if (!boxAnswer.isEmpty()) {
                answer = boxAnswer.toInt();
            } else {
                answer = buttons & 4 ? 6 : 1;   // yes for yes/no, else OK
            }
            QMetaObject::invokeMethod(engine, "onDialogResult", Qt::QueuedConnection,
                                      Q_ARG(int, answer));
            if (input)
                QMetaObject::invokeMethod(engine, "onDialogText", Qt::QueuedConnection,
                                          Q_ARG(QString, boxAnswer.isEmpty()
                                                            ? defaultText
                                                            : QString::fromLocal8Bit(boxAnswer)));
        }
        return;
    }
    emit scriptDialogRequested(prompt, buttons, title, input, defaultText);
}

void Backend::answerScriptDialog(int value, const QString &textValue) {
    DLScriptEngine *engine = m_runner ? m_runner->engine() : nullptr;
    if (!engine)
        return;
    QMetaObject::invokeMethod(engine, "onDialogResult", Qt::QueuedConnection, Q_ARG(int, value));
    if (!textValue.isEmpty())
        QMetaObject::invokeMethod(engine, "onDialogText", Qt::QueuedConnection,
                                  Q_ARG(QString, textValue));
}

// Comments and events join the log files as well, as Docklight does.
void Backend::logComment(const QString &text) {
    m_log->appendComment(text);
    m_logger.onRow(CommLog::Comment, -1, QByteArray(), text,
                   QDateTime::currentMSecsSinceEpoch(), false);
}

void Backend::logEvent(const QString &text) {
    m_log->appendEvent(text);
    m_logger.onRow(CommLog::Event, -1, QByteArray(), text,
                   QDateTime::currentMSecsSinceEpoch(), false);
}

void Backend::clearLog() {
    m_log->clear();
}

void Backend::saveLogTo(const QString &path) {
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit toast(tr("Cannot write %1").arg(path));
        return;
    }
    file.write(m_log->toText().toUtf8());
    if (!file.commit()) {
        emit toast(tr("Cannot write %1").arg(path));
        return;
    }
    m_log->appendEvent(tr("Log saved to %1").arg(path));
    emit toast(tr("Log saved"));
}

void Backend::printLogTo(const QString &path) {
    if (path.isEmpty())
        return;
    QPdfWriter writer(path);
    writer.setTitle(tr("Lucidlight communication window"));
    writer.setCreator("Lucidlight");
    const QPageSize page(QPageSize::A4);
    writer.setPageSize(page);
    const qreal marginMm = 12.0;
    QMarginsF margins(marginMm, marginMm, marginMm, marginMm);
    writer.setPageMargins(margins, QPageLayout::Millimeter);

    QTextDocument doc;
    doc.setDocumentMargin(0);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    doc.setDefaultFont(mono);

    // A small header, then the window content line by line. Monospace keeps
    // the ASCII/HEX/DEC/Binary columns aligned.
    QString html = QStringLiteral("<pre style=\"font-family:'%1';font-size:8pt\">")
                       .arg(mono.family());
    html += tr("Lucidlight communication window — %1\n\n")
                .arg(QDateTime::currentDateTime().toString(
                    QStringLiteral("yyyy-MM-dd hh:mm:ss")));
    const QString text = m_log->toText();
    html += text.toHtmlEscaped();
    html += QStringLiteral("</pre>");
    doc.setHtml(html);
    doc.print(&writer);
    m_log->appendEvent(tr("Communication window printed to %1").arg(path));
    emit toast(tr("Printed to %1").arg(QFileInfo(path).fileName()));
}

void Backend::newProject() {
    m_a->setDevice(QString());
    m_a->setBaud(115200);
    m_a->setDataBits(8);
    m_a->setParity(0);
    m_a->setStopBits(1);
    m_a->setFlow(0);
    m_b->setDevice(QString());
    m_b->setBaud(115200);
    m_sequences->replaceAll({});
    setScriptSource(QString());
    setProjectPath(QString());
    setDirty(false);
}

void Backend::openProjectFrom(const QString &path) {
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit toast(tr("Cannot open %1").arg(path));
        return;
    }
    QJsonParseError parseError;
    const QJsonObject state = QJsonDocument::fromJson(file.readAll(), &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        emit toast(tr("%1: %2").arg(path, parseError.errorString()));
        return;
    }
    QString script;
    bool runOnOpen = false;
    QString scriptFile;
    const QString error = Project::load(state, m_comm, m_a, m_b, m_sequences, &script, m_log,
                                        &runOnOpen, &scriptFile);
    if (!error.isEmpty()) {
        emit toast(error);
        return;
    }
    setScriptSource(script);
    setProjectPath(path);
    m_runOnOpen = runOnOpen;
    emit stateChanged();
    setDirty(false);
    emit toast(tr("Project opened"));
    // Docklight's "run script on project open" option. A script file
    // reference wins over the embedded source.
    if (runOnOpen) {
        if (!scriptFile.isEmpty())
            runScriptFile(scriptFile);
        else if (!m_scriptSource.trimmed().isEmpty())
            runScript(m_scriptSource);
    }
}

void Backend::saveProjectTo(const QString &path) {
    if (path.isEmpty())
        return;
    const QString finalPath = path.endsWith(QLatin1String(".llproj")) ? path : path + ".llproj";
    QSaveFile file(finalPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit toast(tr("Cannot write %1").arg(finalPath));
        return;
    }
    const QJsonObject state =
        Project::save(m_comm, m_a, m_b, m_sequences, m_scriptSource, m_log, m_runOnOpen);
    file.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        emit toast(tr("Cannot write %1").arg(finalPath));
        return;
    }
    setProjectPath(finalPath);
    setDirty(false);
    emit toast(tr("Project saved"));
}

void Backend::saveSession() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
                        + "/lucidlight";
    QDir().mkpath(dir);
    QSaveFile file(dir + "/session.json");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    const QJsonObject state =
        Project::save(m_comm, m_a, m_b, m_sequences, m_scriptSource, m_log, m_runOnOpen);
    file.write(QJsonDocument(state).toJson(QJsonDocument::Compact));
    file.commit();
    setDirty(false);
}

bool Backend::loadSession() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
                        + "/lucidlight";
    QFile file(dir + "/session.json");
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    QString script;
    const QString error = Project::load(state, m_comm, m_a, m_b, m_sequences, &script, m_log);
    if (!error.isEmpty())
        return false;
    setScriptSource(script);
    setProjectPath(QString());
    setDirty(false);
    return true;
}

QString Backend::parseError(const QString &text, int format) const {
    return DataFormat::parse(text, DataFormat::Format(format)).error;
}

QString Backend::previewBytes(const QString &text, int format) const {
    const auto parsed = DataFormat::parse(text, DataFormat::Format(format));
    if (!parsed.ok())
        return parsed.error;
    return DataFormat::toHex(parsed.bytes());
}

void Backend::copyToClipboard(const QString &text) {
    QGuiApplication::clipboard()->setText(text);
    emit toast(tr("Copied"));
}

void Backend::scriptFinished(bool ok, const QString &error) {
    if (ok)
        m_log->appendEvent(tr("Script finished"));
    else
        m_log->appendEvent(error.isEmpty() ? tr("Script stopped") : tr("Script error: %1").arg(error));
    emit toast(ok ? tr("Script finished") : error);
}
