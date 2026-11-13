#include "commlink.h"

#include "netlink.h"

#include "seriallink.h"
#include <QRegularExpression>

CommLink::CommLink(QObject *parent) : QObject(parent) {}

CommLink::~CommLink() = default;

bool CommLink::isNetworkDevice(const QString &device) {
    const QString upper = device.toUpper();
    return upper.startsWith(QLatin1String("TCP:")) || upper.startsWith(QLatin1String("SERVER:"))
           || upper.startsWith(QLatin1String("UDP:")) || upper.startsWith(QLatin1String("UNIX:"))
           || upper.startsWith(QLatin1String("UNIXSERVER:"));
}

CommLink *CommLink::create(const QString &device, QObject *parent) {
    if (isNetworkDevice(device))
        return new NetLink(parent);
    return new SerialLink(parent);
}

QString CommLink::normalizeDevice(const QString &device) {
    const QString trimmed = device.trimmed();
    // Docklight device-string compatibility: a bare "host:port" is a TCP
    // client, e.g. SetChannelSettings "docklight.de:80". Scheme prefixes
    // (SERVER:, TCP:, UDP:, UNIX..., COM ports) are left untouched.
    const QString upper = trimmed.toUpper();
    static const QStringList prefixes = {
        "SERVER:", "TCP:", "UDP:", "UNIX:", "UNIXSERVER:", "TCPCLIENT:",
        "UNIXCLIENT:", "COM", "/DEV/", "PIPE:"
    };
    for (const QString &prefix : prefixes)
        if (upper.startsWith(prefix))
            return device;
    static const QRegularExpression hostPort(
        QStringLiteral("^([^,:\\s]+):(\\d+)$"));
    const QRegularExpressionMatch m = hostPort.match(trimmed);
    if (m.hasMatch())
        return QStringLiteral("TCP:%1:%2").arg(m.captured(1), m.captured(2));
    return device;
}
