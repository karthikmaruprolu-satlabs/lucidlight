#include "commlink.h"

#include "netlink.h"

#include "seriallink.h"

CommLink::CommLink(QObject *parent) : QObject(parent) {}

CommLink::~CommLink() = default;

bool CommLink::isNetworkDevice(const QString &device) {
    const QString upper = device.toUpper();
    return upper.startsWith(QLatin1String("TCP:")) || upper.startsWith(QLatin1String("SERVER:"))
           || upper.startsWith(QLatin1String("UDP:"));
}

CommLink *CommLink::create(const QString &device, QObject *parent) {
    if (isNetworkDevice(device))
        return new NetLink(parent);
    return new SerialLink(parent);
}
