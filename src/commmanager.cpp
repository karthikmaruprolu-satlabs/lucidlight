#include "commmanager.h"

CommManager::CommManager(QObject *parent) : QObject(parent) {
    connect(&m_a, &SerialLink::received, this, [this](const QByteArray &data) { handleIncoming(0, data); });
    connect(&m_b, &SerialLink::received, this, [this](const QByteArray &data) { handleIncoming(1, data); });
    auto reportState = [this](SerialLink &link) {
        if (link.isOpen())
            emit eventLogged(QObject::tr("Port %1 opened").arg(link.device()));
        else if (!link.lastError().isEmpty())
            emit eventLogged(link.lastError());
        emit runningChanged();
    };
    connect(&m_a, &SerialLink::stateChanged, this, [this, reportState] { reportState(m_a); });
    connect(&m_b, &SerialLink::stateChanged, this, [this, reportState] { reportState(m_b); });
}

CommManager::~CommManager() {
    stop();
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
    if (!m_a.open(a))
        return false;
    if (m_mode != Single && !m_b.open(b)) {
        m_a.close();
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
    if (!m_running)
        return;
    m_running = false;
    m_a.close();
    m_b.close();
    emit eventLogged(QObject::tr("Communication stopped"));
    emit runningChanged();
}

bool CommManager::restart() {
    return start(m_lastA, m_lastB);
}

void CommManager::sendBreak(int durationMs) {
    m_a.sendBreak(durationMs);
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
    SerialLink &link = channel == 0 ? m_a : m_b;
    if (!link.isOpen())
        return false;
    const qint64 written = link.write(data);
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
