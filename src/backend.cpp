#include "backend.h"

#include "checksum.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include "project.h"

Backend::Backend(QObject *parent)
    : QObject(parent),
      m_comm(new CommManager(this)),
      m_a(new ChannelSettings(this)),
      m_b(new ChannelSettings(this)),
      m_sequences(new SequenceModel(this)),
      m_log(new CommLog(this)),
      m_runner(new ScriptRunner(m_comm, m_sequences, this)),
      m_ports(new PortEnumerator(this)),
      m_picker(new PortalPicker(this)) {
    connect(m_comm, &CommManager::incoming, this, &Backend::handleIncoming);
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
            m_matcher.reset();
        }
        emit stateChanged();
    });
    connect(m_comm, &CommManager::countersChanged, this, &Backend::stateChanged);
    connect(m_log, &CommLog::displayChanged, this, &Backend::stateChanged);
    connect(&m_matcher, &SequenceMatcher::matched, this, &Backend::handleTrigger);
    connect(m_sequences, &SequenceModel::sequenceChanged, this, &Backend::syncMatcher);
    syncMatcher();

    m_repeatTimer.setInterval(100);
    connect(&m_repeatTimer, &QTimer::timeout, this, &Backend::checkRepeats);

    // Everything below marks the project dirty, so an unsaved-changes prompt
    // can appear before quitting.
    wireDirtyTracking();
    connect(m_runner, &ScriptRunner::finished, this, &Backend::scriptFinished);
    connect(m_runner, &ScriptRunner::commentRequested, m_log, [this](const QString &text) {
        m_log->appendComment(text);
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

void Backend::setScriptSource(const QString &source) {
    if (source == m_scriptSource)
        return;
    m_scriptSource = source;
    emit scriptChanged();
}

// ---------------------------------------------------------------------------
// Traffic
// ---------------------------------------------------------------------------

void Backend::handleIncoming(int channel, const QByteArray &data) {
    // The matcher may fire a trigger for this very burst; remember that so
    // the log row gets the highlight mark.
    m_matcher.feed(data, QDateTime::currentMSecsSinceEpoch());
    const bool highlighted = m_pendingHighlight;
    m_pendingHighlight = false;
    m_log->append(CommLog::Rx, channel, data, highlighted);
}

void Backend::handleTransmitted(int channel, const QByteArray &data) {
    m_log->append(CommLog::Tx, channel, data);
}

void Backend::handleTrigger(int sequenceId, const QVector<SequenceMatcher::Token> &tokens) {
    m_pendingHighlight = true;
    QByteArray received;
    for (const SequenceMatcher::Token &t : tokens)
        if (t.kind == SequenceMatcher::Token::Data)
            received.append(char(t.value));
    for (const Sequence &seq : m_sequences->sequences()) {
        if (seq.id != sequenceId || !seq.enabled)
            continue;
        // Docklight counts every detection since start/reset.
        m_receiveCounts[sequenceId]++;
        applyTrigger(&seq, received);
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
                                : after + 1;
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
        case 'S': replacement = "\a"; break;                  // bell (text form)
        case 'L': replacement = "\n"; break;
        case 'T': replacement = QLocale().toString(QDateTime::currentDateTime(),
                                                    QStringLiteral("HH:mm:ss.zzz"));
            break;
        case 'C': replacement = channel == 0 ? "1" : "2"; break;
        case 'X': replacement = channel == 0 ? "TX" : "RX"; break;
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

void Backend::applyTrigger(const Sequence *receiveSequence, const QByteArray &matched) {
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
        const int channel = m_comm->mode() == CommManager::Monitor ? 1 : 1;
        m_log->appendComment(expandCommentMacros(receiveSequence->comment, receiveSequence,
                                                 matched, channel));
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
    m_matcher.setPatterns(patterns);
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
    } else {
        emit toast(m_comm->link(0)->lastError());
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

void Backend::sendSequence(int row) {
    const Sequence *seq = m_sequences->sequenceAt(row);
    if (!seq || !seq->isSend)
        return;
    const auto parsed = DataFormat::parse(seq->data, seq->format);
    if (!parsed.ok()) {
        emit toast(tr("%1: %2").arg(seq->name, parsed.error));
        return;
    }
    if (!m_comm->send(parsed.bytes())) {
        if (!m_comm->running())
            emit toast(tr("Communication is not running"));
        return;
    }
    if (seq->repeatIntervalMs > 0) {
        if (m_nextRepeatAt.contains(seq->id)) {
            m_nextRepeatAt.remove(seq->id);
            m_log->appendEvent(tr("Repeat stopped: %1").arg(seq->name));
        } else {
            m_nextRepeatAt.insert(seq->id, m_repeatClock.elapsed() + seq->repeatIntervalMs);
            m_log->appendEvent(tr("Repeat every %1 ms: %2").arg(seq->repeatIntervalMs).arg(seq->name));
        }
    }
}

void Backend::sendByHotkey(int keyNumber) {
    const int row = m_sequences->findSendByHotkey(keyNumber);
    if (row >= 0)
        sendSequence(row);
}

void Backend::checkRepeats() {
    if (!m_comm->running()) {
        if (!m_nextRepeatAt.isEmpty())
            m_nextRepeatAt.clear();
        m_repeatTimer.stop();
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
            const auto parsed = DataFormat::parse(seq.data, seq.format);
            if (parsed.ok() && m_comm->send(parsed.bytes()))
                m_nextRepeatAt.insert(seqId, now + seq.repeatIntervalMs);
            break;
        }
    }
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
    const QString error = Project::load(state, m_comm, m_a, m_b, m_sequences, &script, m_log);
    if (!error.isEmpty()) {
        emit toast(error);
        return;
    }
    setScriptSource(script);
    setProjectPath(path);
    setDirty(false);
    emit toast(tr("Project opened"));
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
        Project::save(m_comm, m_a, m_b, m_sequences, m_scriptSource, m_log);
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
        Project::save(m_comm, m_a, m_b, m_sequences, m_scriptSource, m_log);
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
