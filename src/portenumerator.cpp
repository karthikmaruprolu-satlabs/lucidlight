#include "portenumerator.h"

#include <QDir>
#include <QFileInfo>

PortEnumerator::PortEnumerator(QObject *parent) : QObject(parent) {}

QStringList PortEnumerator::ports() const {
    QStringList result;
    // /dev/serial/by-id names describe the adapter, so they come first.
    QDir byId("/dev/serial/by-id");
    const auto entries = byId.entryList(QDir::Files | QDir::System, QDir::Name);
    for (const QString &entry : entries) {
        const QString path = "/dev/serial/by-id/" + entry;
        QFileInfo info(path);
        const QString target = "/dev/" + info.symLinkTarget();
        if (!result.contains(target))
            result.append(path);
    }

    // The usual device node prefixes for real UARTs, USB adapters, and
    // single-board computer ports. Legacy ttyS* come last.
    static const char *patterns[] = {"ttyUSB*", "ttyACM*", "ttyAMA*", "ttyTHS*", "ttyS*"};
    for (const char *pattern : patterns) {
        QDir dev("/dev");
        for (const QString &name : dev.entryList(QStringList(pattern), QDir::System, QDir::Name)) {
            const QString path = "/dev/" + name;
            if (!result.contains(path))
                result.append(path);
        }
    }
    return result;
}
