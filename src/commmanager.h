#pragma once

#include <QObject>
#include <QHash>
#include <atomic>

#include "commlink.h"

// Owns the two serial links and routes bytes according to the communication
// mode, the way Docklight's mode selector does:
//   0 Single   — port A only: send and receive on one line.
//   1 Redirect — what Lucidlight sends goes out on port A; replies are
//                received on port B (breakout-box wiring).
//   2 Monitor  — both ports are read-only taps between two devices; data from
//                port A is what device 1 sent, port B carries device 2's side.
class CommManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(qint64 txBytes READ txBytes NOTIFY countersChanged)
    Q_PROPERTY(qint64 rxBytes READ rxBytes NOTIFY countersChanged)
    // Docklight's Data Forwarding (Monitor mode): RX on one port is
    // re-transmitted on the opposite port's TX line. On by default; the
    // expert option turns it off for pure listening setups.
    Q_PROPERTY(bool dataForwarding READ dataForwarding WRITE setDataForwarding NOTIFY dataForwardingChanged)

public:
    enum Mode { Single = 0, Redirect = 1, Monitor = 2 };
    Q_ENUM(Mode)

    explicit CommManager(QObject *parent = nullptr);
    ~CommManager() override;

    bool running() const { return m_running; }
    int mode() const { return m_mode; }
    void setMode(int mode);
    bool dataForwarding() const { return m_dataForwarding; }
    void setDataForwarding(bool on);
    qint64 txBytes() const { return m_txBytes; }
    qint64 rxBytes() const { return m_rxBytes; }
    void resetCounters();

    // Starts both links as the mode requires. B's device may be empty in
    // single mode.
    bool start(const SerialConfig &a, const SerialConfig &b);
    // Restarts with the configuration of the last start; scripts use this.
    Q_INVOKABLE bool restart();
    Q_INVOKABLE bool applySettings(const SerialConfig &a, const SerialConfig &b);
    // Lucidlight extension: re-open a main channel without stopping the
    // session (auto-reconnect after a serial USB unplug, or retrying a
    // channel that started in the error state).
    Q_INVOKABLE bool reopenChannel(int index);
    Q_INVOKABLE void stop();

    // Sends out of the port the current mode transmits on.
    Q_INVOKABLE bool send(const QByteArray &data);
    // Direct writes for scripted pin-and-break games; bypasses mode routing.
    Q_INVOKABLE bool sendOn(int channel, const QByteArray &data);
    Q_INVOKABLE bool sendOnEx(int channel, const QByteArray &data, bool log);
    Q_INVOKABLE void sendBreak(int durationMs);

    // Side channels 3..10 (stored as indices 2..9), the Docklight scripting
    // extra connections. device is any serial or network device string.
    Q_INVOKABLE bool openSideChannel(int channelNo, const QString &device);
    Q_INVOKABLE void closeSideChannel(int channelNo);
    Q_INVOKABLE bool sideSend(int channelNo, const QByteArray &data);
    Q_INVOKABLE bool sideChannelOpen(int channelNo) const;
    Q_INVOKABLE void setDTR(int channel, bool on);
    Q_INVOKABLE void setRTS(int channel, bool on);

    CommLink *link(int channel) { return channel == 0 ? m_a : m_b; }

    // channelNo 3..10 per Docklight numbering; nullptr when not open.
    CommLink *sideLink(int channelNo) const { return m_side.value(channelNo - 3, nullptr); }

signals:
    void runningChanged();
    void modeChanged();
    void dataForwardingChanged();
    void countersChanged();
    // Data the devices sent; channel is 0 for A, 1 for B.
    void incoming(int channel, const QByteArray &data);
    // A break condition was detected on the given channel (0 = A, 1 = B).
    void breakReceived(int channel);
    // Data Lucidlight itself transmitted.
    void transmitted(int channel, const QByteArray &data);
    void eventLogged(const QString &text);

private:
    void handleIncoming(int channel, const QByteArray &data);
    void attach(CommLink *link, int channel);
    void attachSide(CommLink *link, int index);

    CommLink *m_a = nullptr;   // owned; created per start() from the device string
    CommLink *m_b = nullptr;
    // Side channels 3..10 (indices 2..9); each owned while open.
    QHash<int, CommLink *> m_side;
    std::atomic_bool m_running {false};
    int m_mode = Single;
    bool m_dataForwarding = true;
    qint64 m_txBytes = 0;
    qint64 m_rxBytes = 0;
    SerialConfig m_lastA, m_lastB;
};
