#pragma once

#include <QObject>
#include <QStringList>

// Lists the serial ports on the machine, with the stable by-id names first so
// USB adapters keep the same entry across reboots.
class PortEnumerator : public QObject {
    Q_OBJECT
public:
    explicit PortEnumerator(QObject *parent = nullptr);
    Q_INVOKABLE QStringList ports() const;
};
