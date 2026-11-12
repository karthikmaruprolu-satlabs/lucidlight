#include "netlink.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

NetLink::NetLink(QObject *parent) : CommLink(parent) {}

NetLink::~NetLink() {
    close();
}

void NetLink::setConnected(bool on) {
    if (m_connected == on)
        return;
    m_connected = on;
    emit stateChanged();
}

bool NetLink::open(const SerialConfig &config) {
    close();
    m_device = config.device;
    m_lastError.clear();

    const QString upper = m_device.toUpper();
    if (upper.startsWith(QLatin1String("TCP:"))) {
        m_kind = TcpClient;
        const QString spec = m_device.mid(4);
        const int colon = spec.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0) {
            fail(QObject::tr("TCP device needs TCP:host:port"));
            return false;
        }
        m_host = spec.left(colon);
        m_port = spec.mid(colon + 1).toInt();
        if (m_port <= 0 || m_port > 65535) {
            fail(QObject::tr("TCP device needs a valid port"));
            return false;
        }
        m_tcp = new QTcpSocket(this);
        connectSignals();
        // Bounded synchronous connect keeps CommManager::start's contract.
        m_tcp->connectToHost(m_host, quint16(m_port));
        if (!m_tcp->waitForConnected(3000)) {
            fail(QObject::tr("TCP connect to %1 failed: %2")
                     .arg(m_host, m_tcp->errorString()));
            close();
            return false;
        }
        setConnected(true);
        return true;
    }

    if (upper.startsWith(QLatin1String("SERVER:"))) {
        m_kind = TcpServer;
        m_port = m_device.mid(7).toInt();
        if (m_port <= 0 || m_port > 65535) {
            fail(QObject::tr("SERVER device needs a valid port"));
            return false;
        }
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, [this] {
            if (m_tcp)   // one connection at a time, like a point-to-point link
                return;
            QTcpSocket *client = m_server->nextPendingConnection();
            if (!client)
                return;
            m_tcp = client;
            connectSignals();
            setConnected(true);
        });
        if (!m_server->listen(QHostAddress::Any, quint16(m_port))) {
            fail(QObject::tr("Cannot listen on port %1: %2")
                     .arg(QString::number(m_port), m_server->errorString()));
            close();
            return false;
        }
        m_listening = true;
        emit stateChanged();   // status: waiting for a partner
        return true;
    }

    if (upper.startsWith(QLatin1String("UDP:"))) {
        m_udp = new QUdpSocket(this);
        connectSignals();
        const QString spec = m_device.mid(4);
        const int colon = spec.lastIndexOf(QLatin1Char(':'));
        if (colon > 0) {
            // UDP:host:port — peer with a fixed remote endpoint.
            m_kind = UdpPeer;
            m_host = spec.left(colon);
            m_port = spec.mid(colon + 1).toInt();
            if (m_port <= 0 || m_port > 65535) {
                fail(QObject::tr("UDP device needs a valid port"));
                close();
                return false;
            }
            m_udp->connectToHost(m_host, quint16(m_port));
            if (!m_udp->waitForConnected(3000)) {
                fail(QObject::tr("UDP connect to %1 failed: %2")
                         .arg(m_host, m_udp->errorString()));
                close();
                return false;
            }
        } else {
            // UDP:port — bind locally, receive from anyone.
            m_kind = UdpServer;
            m_port = spec.toInt();
            if (m_port <= 0 || m_port > 65535) {
                fail(QObject::tr("UDP device needs a valid port"));
                close();
                return false;
            }
            if (!m_udp->bind(QHostAddress::Any, quint16(m_port))) {
                fail(QObject::tr("Cannot bind UDP port %1: %2")
                         .arg(QString::number(m_port), m_udp->errorString()));
                close();
                return false;
            }
        }
        setConnected(true);
        return true;
    }

    fail(QObject::tr("Unknown network device %1").arg(m_device));
    return false;
}

void NetLink::connectSignals() {
    if (m_tcp) {
        connect(m_tcp, &QAbstractSocket::readyRead, this, &NetLink::handleReadyRead);
        connect(m_tcp, &QAbstractSocket::disconnected, this, [this] { setConnected(false); });
        connect(m_tcp, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (m_tcp && m_tcp->error() != QAbstractSocket::RemoteHostClosedError)
                fail(m_tcp->errorString());
        });
    }
    if (m_udp) {
        connect(m_udp, &QAbstractSocket::readyRead, this, &NetLink::handleReadyRead);
        connect(m_udp, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (m_udp && m_udp->error() != QAbstractSocket::RemoteHostClosedError)
                fail(m_udp->errorString());
        });
    }
}

void NetLink::close() {
    const bool wasOpen = m_connected || m_listening;
    m_connected = false;
    m_listening = false;
    if (m_tcp) {
        m_tcp->disconnectFromHost();
        if (m_tcp->state() != QAbstractSocket::UnconnectedState)
            m_tcp->waitForDisconnected(1000);
        m_tcp->deleteLater();
        m_tcp = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    if (m_udp) {
        m_udp->close();
        m_udp->deleteLater();
        m_udp = nullptr;
    }
    if (wasOpen)
        emit stateChanged();
}

void NetLink::handleReadyRead() {
    if (m_tcp) {
        while (m_tcp->bytesAvailable() > 0)
            emit received(m_tcp->read(m_tcp->bytesAvailable()));
        return;
    }
    if (m_udp) {
        while (m_udp->hasPendingDatagrams()) {
            QByteArray datagram;
            datagram.resize(int(m_udp->pendingDatagramSize()));
            QHostAddress sender;
            quint16 senderPort = 0;
            m_udp->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
            if (m_kind == UdpServer && !m_host.isEmpty()) {
                // A UDP server answers whoever sent last.
            } else if (m_kind == UdpServer) {
                m_peerAddress = sender;
                m_peerPort = senderPort;
            }
            emit received(datagram);
        }
    }
}

qint64 NetLink::write(const QByteArray &data) {
    if (!m_connected)
        return -1;
    if (m_tcp) {
        const qint64 written = m_tcp->write(data);
        if (written > 0)
            m_tcp->flush();
        return written;
    }
    if (m_udp) {
        if (m_kind == UdpServer && !m_peerAddress.isNull()) {
            return m_udp->writeDatagram(data, m_peerAddress, quint16(m_peerPort));
        }
        return m_udp->write(data);
    }
    return -1;
}

void NetLink::fail(const QString &message) {
    m_lastError = message;
    emit stateChanged();
}
