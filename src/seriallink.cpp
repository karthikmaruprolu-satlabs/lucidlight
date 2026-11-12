#include "seriallink.h"

#include <QElapsedTimer>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

SerialLink::SerialLink(QObject *parent) : CommLink(parent) {}

SerialLink::~SerialLink() {
    close();
}

// Map a numeric baud rate to a termios speed constant. Rates beyond the
// standard constants are simply refused at open time with a clear message,
// instead of silently running at a wrong speed.
speed_t SerialLink::speedConstant(int baud, bool *known) {
    static const struct { int rate; speed_t constant; } table[] = {
        {110, B110},     {300, B300},     {600, B600},     {1200, B1200},   {2400, B2400},
        {4800, B4800},   {9600, B9600},   {14400, B14400}, {19200, B19200}, {28800, B28800},
        {38400, B38400}, {57600, B57600}, {115200, B115200}, {230400, B230400},
        {460800, B460800}, {500000, B500000}, {576000, B576000}, {921600, B921600},
        {1000000, B1000000}, {1152000, B1152000}, {1500000, B1500000}, {2000000, B2000000},
        {2500000, B2500000}, {3000000, B3000000}, {3500000, B3500000}, {4000000, B4000000},
    };
    for (const auto &row : table)
        if (row.rate == baud) {
            if (known)
                *known = true;
            return row.constant;
        }
    if (known)
        *known = false;
    return B0;
}

bool SerialLink::open(const SerialConfig &config) {
    close();
    bool known = false;
    speedConstant(config.baud, &known);
    if (!known) {
        fail(QObject::tr("Unsupported baud rate %1").arg(config.baud));
        return false;
    }
    m_fd = ::open(config.device.toLocal8Bit().constData(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        fail(QObject::tr("Cannot open %1: %2").arg(config.device, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    if (::tcgetattr(m_fd, &m_savedTermios) == 0)
        m_savedValid = true;

    m_device = config.device;
    m_lastError.clear();
    applyConfig(config);

    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, [this] { readReady(); });
    emit stateChanged();
    return true;
}

void SerialLink::close() {
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        ::tcflush(m_fd, TCIOFLUSH);
        if (m_savedValid)
            ::tcsetattr(m_fd, TCSANOW, &m_savedTermios);
        ::close(m_fd);
        m_fd = -1;
        m_device.clear();
        m_savedValid = false;
        emit stateChanged();
    }
}

void SerialLink::applyConfig(const SerialConfig &config) {
    termios tio {};
    ::cfmakeraw(&tio);

    const bool known = true;
    const speed_t speed = speedConstant(config.baud, const_cast<bool *>(&known));
    ::cfsetispeed(&tio, speed);
    ::cfsetospeed(&tio, speed);

    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CSIZE;
    switch (config.dataBits) {
    case 5: tio.c_cflag |= CS5; break;
    case 6: tio.c_cflag |= CS6; break;
    case 7: tio.c_cflag |= CS7; break;
    default: tio.c_cflag |= CS8; break;
    }

    // Mark and space parity need the CMSPAR extension, which Linux provides.
    tio.c_cflag &= ~(PARENB | PARODD | CMSPAR);
    m_parity = config.parity;
    switch (config.parity) {
    case 1: tio.c_cflag |= PARENB; break;                       // even
    case 2: tio.c_cflag |= PARENB | PARODD; break;              // odd
    case 3: tio.c_cflag |= PARENB | PARODD | CMSPAR; break;     // mark
    case 4: tio.c_cflag |= PARENB | CMSPAR; break;              // space
    default: break;                                             // none
    }

    if (config.stopBits == 2)
        tio.c_cflag |= CSTOPB;
    else
        tio.c_cflag &= ~CSTOPB;

    tio.c_cflag &= ~CRTSCTS;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    if (config.flow == 1)
        tio.c_cflag |= CRTSCTS;
    else if (config.flow == 2)
        tio.c_iflag |= IXON | IXOFF;

    // Non-blocking reads: the socket notifier tells us when data arrived, so
    // VMIN/VTIME stay at zero.
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if (::tcsetattr(m_fd, TCSANOW, &tio) != 0)
        m_lastError = QString::fromLocal8Bit(strerror(errno));
    ::tcflush(m_fd, TCIOFLUSH);
}

void SerialLink::fail(const QString &message) {
    m_lastError = message;
    emit stateChanged();
}

void SerialLink::readReady() {
    if (m_fd < 0)
        return;
    QByteArray chunk;
    chunk.resize(8192);
    qint64 total = 0;
    while (true) {
        const qint64 n = ::read(m_fd, chunk.data() + total, chunk.size() - total);
        if (n > 0) {
            total += n;
            if (total == chunk.size())
                chunk.resize(chunk.size() * 2);
            continue;
        }
        break;
    }
    if (total > 0) {
        chunk.resize(int(total));
        emit received(chunk);
    }
    if (total == 0 && errno == EAGAIN)
        return;
}

qint64 SerialLink::write(const QByteArray &data) {
    if (m_fd < 0 || data.isEmpty())
        return 0;
    qint64 written = 0;
    QElapsedTimer deadline;
    deadline.start();
    while (written < data.size() && deadline.elapsed() < 2000) {
        const qint64 n = ::write(m_fd, data.constData() + written, data.size() - written);
        if (n > 0) {
            written += n;
            continue;
        }
        if (n < 0 && errno == EAGAIN) {
            ::usleep(2000);
            continue;
        }
        break;
    }
    ::tcdrain(m_fd);
    return written;
}

void SerialLink::sendBreak(int durationMs) {
    if (m_fd < 0)
        return;
    ::tcsendbreak(m_fd, qBound(1, durationMs / 100 + 1, 40));
}

bool SerialLink::setDTR(bool on) {
    if (m_fd < 0)
        return false;
    int flags = 0;
    if (::ioctl(m_fd, TIOCMGET, &flags) != 0)
        return false;
    if (on)
        flags |= TIOCM_DTR;
    else
        flags &= ~TIOCM_DTR;
    return ::ioctl(m_fd, TIOCMSET, &flags) == 0;
}

bool SerialLink::setRTS(bool on) {
    if (m_fd < 0)
        return false;
    int flags = 0;
    if (::ioctl(m_fd, TIOCMGET, &flags) != 0)
        return false;
    if (on)
        flags |= TIOCM_RTS;
    else
        flags &= ~TIOCM_RTS;
    return ::ioctl(m_fd, TIOCMSET, &flags) == 0;
}

// A set TIOCM_CTS bit means the line is asserted on Linux.
bool SerialLink::setParity(int parity) {
    if (m_fd < 0)
        return false;
    termios tio {};
    if (::tcgetattr(m_fd, &tio) != 0)
        return false;
    tio.c_cflag &= ~(PARENB | PARODD | CMSPAR);
    switch (parity) {
    case 1: tio.c_cflag |= PARENB; break;                       // even
    case 2: tio.c_cflag |= PARENB | PARODD; break;              // odd
    case 3: tio.c_cflag |= PARENB | PARODD | CMSPAR; break;     // mark
    case 4: tio.c_cflag |= PARENB | CMSPAR; break;              // space
    default: break;                                             // none
    }
    if (::tcsetattr(m_fd, TCSANOW, &tio) == 0) {
        m_parity = parity;
        return true;
    }
    return false;
}

bool SerialLink::cts() const {
    int flags = 0;
    return m_fd >= 0 && ::ioctl(m_fd, TIOCMGET, &flags) == 0 && (flags & TIOCM_CTS);
}
bool SerialLink::dsr() const {
    int flags = 0;
    return m_fd >= 0 && ::ioctl(m_fd, TIOCMGET, &flags) == 0 && (flags & TIOCM_DSR);
}
bool SerialLink::dcd() const {
    int flags = 0;
    return m_fd >= 0 && ::ioctl(m_fd, TIOCMGET, &flags) == 0 && (flags & TIOCM_CAR);
}
bool SerialLink::ri() const {
    int flags = 0;
    return m_fd >= 0 && ::ioctl(m_fd, TIOCMGET, &flags) == 0 && (flags & TIOCM_RNG);
}
