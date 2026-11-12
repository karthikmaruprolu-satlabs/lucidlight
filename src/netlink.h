#pragma once

#include <QHostAddress>
#include <QString>

#include "commlink.h"

class QTcpServer;
class QTcpSocket;
class QUdpSocket;

// The Docklight network channels:
//   TCP:host:port  — client, connects to a listening device
//   SERVER:port    — listens and accepts a single connection (status shows
//                    "waiting" until a partner connects, like Docklight)
//   UDP:host:port  — peer with a fixed remote endpoint
//   UDP:port       — bound locally, accepts datagrams from anywhere
class NetLink : public CommLink {
    Q_OBJECT
public:
    enum Kind { TcpClient, TcpServer, UdpPeer, UdpServer };

    explicit NetLink(QObject *parent = nullptr);
    ~NetLink() override;

    bool open(const SerialConfig &config) override;
    void close() override;

    bool isOpen() const override { return m_connected; }
    bool isWaiting() const { return m_listening && !m_connected; }
    QString device() const override { return m_device; }
    QString lastError() const override { return m_lastError; }

    qint64 write(const QByteArray &data) override;

private:
    void connectSignals();
    void handleReadyRead();
    void fail(const QString &message);
    void setConnected(bool on);

    Kind m_kind = TcpClient;
    QString m_device;
    QString m_lastError;
    QString m_host;
    int m_port = 0;
    bool m_connected = false;
    bool m_listening = false;
    QTcpSocket *m_tcp = nullptr;
    QTcpServer *m_server = nullptr;
    QUdpSocket *m_udp = nullptr;
    QHostAddress m_peerAddress;
    int m_peerPort = 0;
};
