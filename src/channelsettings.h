#pragma once

#include <QObject>
#include <QString>

#include "seriallink.h"

// Settings for one communication channel (port A or port B), exposed to QML
// so the settings popup can bind directly, and serialized into projects.
class ChannelSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString device READ device WRITE setDevice NOTIFY changed)
    Q_PROPERTY(int baud READ baud WRITE setBaud NOTIFY changed)
    Q_PROPERTY(int dataBits READ dataBits WRITE setDataBits NOTIFY changed)
    Q_PROPERTY(int parity READ parity WRITE setParity NOTIFY changed)
    Q_PROPERTY(int stopBits READ stopBits WRITE setStopBits NOTIFY changed)
    Q_PROPERTY(int flow READ flow WRITE setFlow NOTIFY changed)
    Q_PROPERTY(int parityErrorChar READ parityErrorChar WRITE setParityErrorChar NOTIFY changed)

public:
    explicit ChannelSettings(QObject *parent = nullptr) : QObject(parent) {}

    QString device() const { return m_device; }
    void setDevice(const QString &device) { if (device != m_device) { m_device = device; emit changed(); } }
    int baud() const { return m_baud; }
    void setBaud(int baud) { if (baud != m_baud) { m_baud = baud; emit changed(); } }
    int dataBits() const { return m_dataBits; }
    void setDataBits(int bits) { if (bits != m_dataBits) { m_dataBits = bits; emit changed(); } }
    int parity() const { return m_parity; }
    void setParity(int parity) { if (parity != m_parity) { m_parity = parity; emit changed(); } }
    int stopBits() const { return m_stopBits; }
    void setStopBits(int bits) { if (bits != m_stopBits) { m_stopBits = bits; emit changed(); } }
    int flow() const { return m_flow; }
    void setFlow(int flow) { if (flow != m_flow) { m_flow = flow; emit changed(); } }
    int parityErrorChar() const { return m_parityErrorChar; }
    void setParityErrorChar(int ch) {
        if (ch != m_parityErrorChar) { m_parityErrorChar = ch; emit changed(); }
    }

    SerialConfig config() const {
        return {m_device, m_baud, m_dataBits, m_parity, m_stopBits, m_flow, m_parityErrorChar};
    }

signals:
    void changed();

private:
    QString m_device;
    int m_baud = 115200;
    int m_dataBits = 8;
    int m_parity = 0;
    int m_stopBits = 1;
    int m_flow = 0;
    int m_parityErrorChar = int('?');
};
