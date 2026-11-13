#include "decoder.h"

#include <QStringList>

namespace ModbusDecoder {

// CRC-16/MODBUS, bit-by-bit: poly 0xA001 (reflected), init 0xFFFF, no
// final xor. Wire order: low byte first.
static quint16 modbusCrc(const QByteArray &data) {
    quint16 crc = 0xFFFF;
    for (uchar byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? quint16((crc >> 1) ^ 0xA001) : quint16(crc >> 1);
    }
    return crc;
}

static QString functionName(int fc) {
    switch (fc) {
    case 1: return QStringLiteral("read-coils");
    case 2: return QStringLiteral("read-discrete-inputs");
    case 3: return QStringLiteral("read-holding-regs");
    case 4: return QStringLiteral("read-input-regs");
    case 5: return QStringLiteral("write-single-coil");
    case 6: return QStringLiteral("write-single-reg");
    case 7: return QStringLiteral("read-exception-status");
    case 8: return QStringLiteral("diagnostics");
    case 11: return QStringLiteral("get-comm-event-counter");
    case 12: return QStringLiteral("get-comm-event-log");
    case 15: return QStringLiteral("write-multi-coils");
    case 16: return QStringLiteral("write-multi-regs");
    case 17: return QStringLiteral("report-server-id");
    case 22: return QStringLiteral("read-write-multiple-regs");
    case 23: return QStringLiteral("mask-write-reg");
    default: break;
    }
    if (fc & 0x80)
        return QStringLiteral("exception-response");
    return QString();
}

static int expectedLength(int fc, const uchar *data, int size) {
    // Returns the full frame length (addr + fc + pdu + crc2) for request/
    // response shapes, or -1 when the shape is not deterministic.
    auto fixed = [](int n) { return n; };
    switch (fc) {
    case 1: case 2: case 3: case 4:
        if (size >= 5) {
            const int count = (data[2] << 8) | data[3];
            // Request: fixed 8 bytes. Response: byteCount header.
            return size == 8 ? fixed(8) : 5 + data[2];
        }
        return -1;
    case 5: case 6:
        return fixed(8);
    case 7: case 11:
        return fixed(5);
    case 12:
        return -1;
    case 15: case 16:
        if (size >= 7)
            return 9 + data[6];   // addr fc start(2) count(2) byteCount data... crc
        return -1;
    case 22: case 23:
        return fixed(0x12);
    default:
        if (fc & 0x80)
            return fixed(5);      // exception: addr fc code crc
        return -1;
    }
}

QString tryDecode(const QByteArray &frame) {
    const int size = frame.size();
    if (size < 4 || size > 512)
        return QString();
    // CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF) over everything
    // except the trailing two bytes, little-endian on the wire.
    const QByteArray payload = frame.left(size - 2);
    const quint16 crc = modbusCrc(payload);
    if (uchar(frame[size - 2]) != (crc & 0xFF)
        || uchar(frame[size - 1]) != (crc >> 8))
        return QString();

    const uchar *d = reinterpret_cast<const uchar *>(frame.constData());
    const int addr = d[0];
    const int fc = d[1];
    if (addr < 1 || addr > 247)
        return QString();
    const QString fn = functionName(fc);
    if (fn.isEmpty())
        return QString();
    if (fc & 0x80)
        return QStringLiteral("MODBUS addr=%1 FC=%2 code=%3 CRC ok")
            .arg(addr).arg(fc & 0x7F).arg(d[2]);

    QString detail;
    switch (fc) {
    case 1: case 2: case 3: case 4: {
        if (size == 8) {
            const int start = (d[2] << 8) | d[3];
            const int count = (d[4] << 8) | d[5];
            detail = QStringLiteral("start=%1 count=%2").arg(start).arg(count);
        } else if (size >= 5) {
            const int byteCount = d[2];
            QStringList values;
            for (int i = 0; i < byteCount && 4 + i < size - 2; i += 2) {
                const int v = (d[3 + i] << 8) | d[4 + i];
                values << QString::number(v);
            }
            detail = QStringLiteral("%1 regs: %2")
                         .arg(byteCount / 2)
                         .arg(values.join(QLatin1Char(' ')));
        }
        break;
    }
    case 5: case 6: {
        const int value = (d[4] << 8) | d[5];
        const int ref = (d[2] << 8) | d[3];
        detail = QStringLiteral("ref=%1 value=%2").arg(ref).arg(value);
        break;
    }
    case 15: case 16: {
        const int start = (d[2] << 8) | d[3];
        const int count = (d[4] << 8) | d[5];
        detail = QStringLiteral("start=%1 count=%2").arg(start).arg(count);
        break;
    }
    default:
        break;
    }
    return QStringLiteral("MODBUS addr=%1 FC=%2 %3%4 CRC ok")
        .arg(addr).arg(fc).arg(fn, detail.isEmpty() ? QString() : QLatin1Char(' ') + detail);
}

} // namespace ModbusDecoder
