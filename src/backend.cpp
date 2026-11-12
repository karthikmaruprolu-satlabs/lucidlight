#include "backend.h"

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
    m_matcher.feed(data);
    const bool highlighted = m_pendingHighlight;
    m_pendingHighlight = false;
    m_log->append(CommLog::Rx, channel, data, highlighted);
}

void Backend::handleTransmitted(int channel, const QByteArray &data) {
    m_log->append(CommLog::Tx, channel, data);
}

void Backend::handleTrigger(int sequenceId, const QByteArray &matched) {
    m_pendingHighlight = true;
    for (const Sequence &seq : m_sequences->sequences()) {
        if (seq.id != sequenceId || !seq.enabled)
            continue;
        applyTrigger(&seq, matched);
        break;
    }
}

void Backend::applyTrigger(const Sequence *receiveSequence, const QByteArray &matched) {
    if (receiveSequence->replyToId < 0)
        return;
    for (const Sequence &candidate : m_sequences->sequences()) {
        if (candidate.id != receiveSequence->replyToId || !candidate.isSend)
            continue;
        const auto parsed = DataFormat::parse(candidate.data, candidate.format);
        if (!parsed.ok()) {
            m_log->appendEvent(tr("Trigger %1: reply sequence has invalid data")
                                   .arg(receiveSequence->name));
            return;
        }
        if (m_comm->send(parsed.bytes))
            m_log->appendEvent(tr("Trigger %1 → sent %2").arg(receiveSequence->name, candidate.name));
        else
            m_log->appendEvent(tr("Trigger %1: send failed").arg(receiveSequence->name));
        return;
    }
    m_log->appendEvent(tr("Trigger %1: reply sequence missing").arg(receiveSequence->name));
    Q_UNUSED(matched)
}

// Rebuild the receive-trigger patterns from the sequence table. Called at
// startup and whenever the table changes.
void Backend::syncMatcher() {
    QVector<SequenceMatcher::Pattern> patterns;
    for (const Sequence &seq : m_sequences->sequences()) {
        if (seq.isSend || !seq.enabled)
            continue;
        const auto parsed = DataFormat::parse(seq.data, seq.format);
        if (!parsed.ok() || parsed.bytes.isEmpty())
            continue;
        SequenceMatcher::Pattern pattern;
        pattern.sequenceId = seq.id;
        pattern.bytes = parsed.bytes;
        pattern.wildcard = parsed.wildcard;
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
    if (!m_comm->send(parsed.bytes)) {
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
    if (!m_comm->send(parsed.bytes)) {
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
            if (parsed.ok() && m_comm->send(parsed.bytes))
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
    return DataFormat::toHex(parsed.bytes);
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
