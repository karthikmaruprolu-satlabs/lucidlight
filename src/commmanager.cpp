#include "commmanager.h"

#include <QDebug>

CommManager::CommManager(QObject *parent) : QObject(parent) {}

CommManager::~CommManager() {
    stop();
    delete m_a;
    delete m_b;
}

// Wires one freshly created link into the routing.
void CommManager::attach(CommLink *link, int channel) {
    connect(link, &CommLink::received, this,
            [this, channel](const QByteArray &data) { handleIncoming(channel, data); });
    connect(link, &CommLink::stateChanged, this, [this, link, channel] {
        if (link->isOpen())
            emit eventLogged(QObject::tr("Port %1 opened").arg(link->device()));
        else if (!link->lastError().isEmpty())
            emit eventLogged(link->lastError());
        else if (link->device().startsWith("SERVER:"))
            emit eventLogged(QObject::tr("Port %1 waiting for a connection").arg(link->device()));
        emit runningChanged();
        Q_UNUSED(channel)
    });
}

void CommManager::setMode(int mode) {
    if (mode == m_mode || mode < Single || mode > Monitor)
        return;
    m_mode = mode;
    emit modeChanged();
}

void CommManager::resetCounters() {
    m_txBytes = 0;
    m_rxBytes = 0;
    emit countersChanged();
}

bool CommManager::start(const SerialConfig &a, const SerialConfig &b) {
    stop();
    if (a.device.isEmpty()) {
        emit eventLogged(QObject::tr("Pick a device for port A first"));
        return false;
    }
    if (m_mode != Single && b.device.isEmpty()) {
        emit eventLogged(QObject::tr("This mode needs a device for port B"));
        return false;
    }
    m_lastA = a;
    m_lastB = b;
    m_txBytes = 0;
    m_rxBytes = 0;
    m_a = CommLink::create(a.device, this);
    m_b = CommLink::create(b.device, this);
    attach(m_a, 0);
    attach(m_b, 1);
    if (!m_a->open(a))
        return false;
    if (m_mode != Single && !m_b->open(b)) {
        m_a->close();
        return false;
    }
    m_running = true;
    emit eventLogged(m_mode == Single ? QObject::tr("Communication started (single port)")
               : m_mode == Redirect ? QObject::tr("Communication started (redirect)")
                                    : QObject::tr("Communication started (monitor)"));
    emit countersChanged();
    emit runningChanged();
    return true;
}

void CommManager::stop() {
    qDeleteAll(m_side);
    m_side.clear();
    if (!m_running)
        return;
    m_running = false;
    if (m_a)
        m_a->close();
    if (m_b)
        m_b->close();
    delete m_a;
    delete m_b;
    m_a = m_b = nullptr;
    emit eventLogged(QObject::tr("Communication stopped"));
    emit runningChanged();
}

bool CommManager::restart() {
    return start(m_lastA, m_lastB);
}

bool CommManager::openSideChannel(int channelNo, const QString &device) {
    // Docklight numbers side channels 3..10; indices 2..9 internally.
    const int index = channelNo - 3;
    if (index < 0 || index > 7) {
        emit eventLogged(QObject::tr("Side channel numbers run from 3 to 10"));
        return false;
    }
    if (m_side.contains(index)) {
        m_side.take(index)->deleteLater();
    }
    SerialConfig config;
    config.device = device;
    CommLink *link = CommLink::create(device, this);
    attachSide(link, index);
    if (!link->open(config)) {
        link->deleteLater();
        return false;
    }
    m_side[index] = link;
    emit eventLogged(QObject::tr("Side channel %1 opened on %2").arg(channelNo).arg(device));
    return true;
}

void CommManager::closeSideChannel(int channelNo) {
    const int index = channelNo - 3;
    if (!m_side.contains(index))
        return;
    m_side.take(index)->deleteLater();
    emit eventLogged(QObject::tr("Side channel %1 closed").arg(channelNo));
}

bool CommManager::sideSend(int channelNo, const QByteArray &data) {
    const int index = channelNo - 3;
    CommLink *link = m_side.value(index, nullptr);
    if (!link || !link->isOpen())
        return false;
    const qint64 written = link->write(data);
    return written == data.size();
}

bool CommManager::sideChannelOpen(int channelNo) const {
    const CommLink *link = m_side.value(channelNo - 3, nullptr);
    return link && link->isOpen();
}

void CommManager::attachSide(CommLink *link, int index) {
    // Side channel data flows into the normal incoming path with its channel
    // number, 2..9, so the matcher and log see it like any other line.
    connect(link, &CommLink::received, this,
            [this, index](const QByteArray &data) { handleIncoming(index + 2, data); });
    connect(link, &CommLink::stateChanged, this, [this, link, index] {
        if (!link->isOpen() && !m_side.contains(index))
            return;
        if (!link->isOpen())
            emit eventLogged(QObject::tr("Side channel %1 disconnected").arg(index + 3));
        emit runningChanged();
    });
}

void CommManager::sendBreak(int durationMs) {
    if (m_a)
        m_a->sendBreak(durationMs);
}

void CommManager::setDTR(int channel, bool on) {
    link(channel)->setDTR(on);
}

void CommManager::setRTS(int channel, bool on) {
    link(channel)->setRTS(on);
}

bool CommManager::send(const QByteArray &data) {
    if (!m_running || data.isEmpty())
        return false;
    // Single and redirect both transmit on port A; monitor mode is read-only.
    if (m_mode == Monitor) {
        emit eventLogged(QObject::tr("Monitor mode does not transmit"));
        return false;
    }
    return sendOn(0, data);
}

bool CommManager::sendOn(int channel, const QByteArray &data) {
    CommLink *link = channel == 0 ? m_a : m_b;
    if (!link || !link->isOpen())
        return false;
    const qint64 written = link->write(data);
    if (written <= 0)
        return false;
    m_txBytes += written;
    emit countersChanged();
    emit transmitted(channel, data.left(int(written)));
    return written == data.size();
}

void CommManager::handleIncoming(int channel, const QByteArray &data) {
    if (!m_running)
        return;
    m_rxBytes += data.size();
    emit countersChanged();
    emit incoming(channel, data);
}
