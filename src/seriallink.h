#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <termios.h>

// One serial line, opened with plain POSIX termios so the app only needs
// qt6-base and qt6-declarative. Handles the settings Docklight offers: baud
// rate, data bits, parity (including mark/space), stop bits, and flow
// control, plus the modem lines and the break signal for scripted tests.
struct SerialConfig {
    QString device;
    int baud = 115200;
    int dataBits = 8;          // 5..8
    int parity = 0;            // 0 none, 1 even, 2 odd, 3 mark, 4 space
    int stopBits = 1;          // 1 or 2
    int flow = 0;              // 0 none, 1 hardware RTS/CTS, 2 software Xon/Xoff
    bool operator==(const SerialConfig &o) const {
        return device == o.device && baud == o.baud && dataBits == o.dataBits && parity == o.parity
               && stopBits == o.stopBits && flow == o.flow;
    }
};

class SerialLink : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen NOTIFY stateChanged)
    Q_PROPERTY(QString device READ device NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)

public:
    explicit SerialLink(QObject *parent = nullptr);
    ~SerialLink() override;

    bool isOpen() const { return m_fd >= 0; }
    QString device() const { return m_device; }
    QString lastError() const { return m_lastError; }

    bool open(const SerialConfig &config);
    void close();

    qint64 write(const QByteArray &data);
    // Break and modem lines, for scripted sequences that drive handshake pins.
    void sendBreak(int durationMs);
    bool setDTR(bool on);
    bool setRTS(bool on);
    // Switches the parity setting of an open port on the fly, used by the
    // '!' function character for 9-bit protocols. Returns false on failure.
    bool setParity(int parity);
    int parity() const { return m_parity; }
    bool cts() const;
    bool dsr() const;
    bool dcd() const;
    bool ri() const;

    // Human-readable Bxxxx constant for a numeric baud rate.
    static speed_t speedConstant(int baud, bool *known);

signals:
    void received(const QByteArray &data);
    void stateChanged();

private:
    void readReady();
    void applyConfig(const SerialConfig &config);
    void fail(const QString &message);

    int m_fd = -1;
    QString m_device;
    QString m_lastError;
    int m_parity = 0;
    termios m_savedTermios {};
    bool m_savedValid = false;
    QSocketNotifier *m_notifier = nullptr;
};
