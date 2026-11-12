#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <termios.h>

#include "commlink.h"

// One serial line, opened with plain POSIX termios so the app only needs
// qt6-base and qt6-declarative. Handles the settings Docklight offers: baud
// rate, data bits, parity (including mark/space), stop bits, and flow
// control, plus the modem lines and the break signal for scripted tests.
class SerialLink : public CommLink {
    Q_OBJECT

public:
    explicit SerialLink(QObject *parent = nullptr);
    ~SerialLink() override;

    bool isOpen() const override { return m_fd >= 0; }
    QString device() const override { return m_device; }
    QString lastError() const override { return m_lastError; }

    bool open(const SerialConfig &config) override;
    void close() override;

    qint64 write(const QByteArray &data) override;
    // Break and modem lines, for scripted sequences that drive handshake pins.
    void sendBreak(int durationMs) override;
    bool setDTR(bool on) override;
    bool setRTS(bool on) override;
    // Switches the parity setting of an open port on the fly, used by the
    // '!' function character for 9-bit protocols. Returns false on failure.
    bool setParity(int parity) override;
    int parity() const override { return m_parity; }
    bool cts() const override;
    bool dsr() const override;
    bool dcd() const override;
    bool ri() const override;

    // Human-readable Bxxxx constant for a numeric baud rate.
    static speed_t speedConstant(int baud, bool *known);

    // Decodes PARMRK escapes: "\377 \0 X" is a byte received with a parity
    // error and becomes the ParityErrorChar; "\377 \377" is a real 0xFF
    // data byte. Public so tests can exercise it without hardware.
    static QByteArray decodeParity(const QByteArray &raw, int parityErrorChar);

private:
    void readReady();
    void applyConfig(const SerialConfig &config);
    void fail(const QString &message);

    int m_fd = -1;
    QString m_device;
    QString m_lastError;
    int m_parity = 0;
    int m_parityErrorChar = int('?');
    bool m_decodeParity = false;   // PARMRK decoding active (parity + error char)
    termios m_savedTermios {};
    bool m_savedValid = false;
    QSocketNotifier *m_notifier = nullptr;
};
