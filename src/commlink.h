#pragma once

#include <QObject>
#include <QString>

// The settings shared by every transport. Network links only use the device
// string; the serial fields apply to /dev/tty* channels.
struct SerialConfig {
    QString device;
    int baud = 115200;
    int dataBits = 8;          // 5..8
    int parity = 0;            // 0 none, 1 even, 2 odd, 3 mark, 4 space
    int stopBits = 1;          // 1 or 2
    int flow = 0;              // 0 none, 1 hardware RTS/CTS, 2 software Xon/Xoff
    int parityErrorChar = int('?');   // Docklight's ParityErrorChar replacement
    bool operator==(const SerialConfig &o) const {
        return device == o.device && baud == o.baud && dataBits == o.dataBits && parity == o.parity
               && stopBits == o.stopBits && flow == o.flow
               && parityErrorChar == o.parityErrorChar;
    }
};

// Abstract transport behind one communication channel. SerialLink covers the
// /dev/tty* world; NetLink adds the Docklight network channels (TCP client,
// TCP server, UDP peer, UDP server). CommManager only talks to this
// interface, so a channel can be serial or network without the rest of the
// app caring.
class CommLink : public QObject {
    Q_OBJECT
public:
    explicit CommLink(QObject *parent = nullptr);
    ~CommLink() override;

    virtual bool open(const SerialConfig &config) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual QString device() const = 0;
    virtual QString lastError() const = 0;
    virtual qint64 write(const QByteArray &data) = 0;

    // Serial-only extras; network links answer with safe no-ops.
    virtual void sendBreak(int durationMs) { Q_UNUSED(durationMs) }
    virtual bool setDTR(bool on) { Q_UNUSED(on) return false; }
    virtual bool setRTS(bool on) { Q_UNUSED(on) return false; }
    virtual bool setParity(int parity) { Q_UNUSED(parity) return false; }
    virtual int parity() const { return 0; }
    virtual bool cts() const { return true; }
    virtual bool dsr() const { return true; }
    virtual bool dcd() const { return false; }
    virtual bool ri() const { return false; }

    // Recognizes the Docklight-style network device strings:
    //   TCP:host:port   connect as a client
    //   SERVER:port     listen and accept one connection
    //   UDP:host:port   peer with a fixed remote endpoint
    //   UDP:port        bind locally, receive from anyone
    static bool isNetworkDevice(const QString &device);
    // "host:port" → "TCP:host:port" (Docklight device-string compatibility)
    static QString normalizeDevice(const QString &device);
    // Factory for the transport a device string asks for. The caller owns
    // the link.
    static CommLink *create(const QString &device, QObject *parent);

signals:
    void received(const QByteArray &data);
    void stateChanged();
};
