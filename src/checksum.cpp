#include "checksum.h"

#include <QRegularExpression>

namespace {

quint32 reflect(quint32 value, int bits) {
    quint32 out = 0;
    for (int i = 0; i < bits; ++i) {
        if (value & (1u << i))
            out |= 1u << (bits - 1 - i);
    }
    return out;
}

// Renders checksum bytes as readable ASCII hex text: byte 0x7E becomes the
// two characters "7E".
QByteArray asciiHex(const QByteArray &bytes, bool littleEndian) {
    QByteArray out;
    out.reserve(bytes.size() * 2);
    for (int i = 0; i < bytes.size(); ++i) {
        const int idx = littleEndian ? bytes.size() - 1 - i : i;
        out.append(QString::asprintf("%02X", uchar(bytes.at(idx))).toLatin1());
    }
    return out;
}

// Length of the checksum result in bytes.
int specBytes(const Checksum::Spec &spec) {
    switch (spec.kind) {
    case Checksum::Spec::Mod256:
    case Checksum::Spec::Xor:
    case Checksum::Spec::Lrc:
    case Checksum::Spec::LrcAscii:
        return 1;
    case Checksum::Spec::Mod65536:
        return 2;
    default:
        return spec.width > 0 ? (spec.width + 7) / 8 : 0;
    }
}

}  // namespace

Checksum::Spec Checksum::parse(const QString &spec) {
    Spec result;
    const QString trimmed = spec.trimmed();
    if (trimmed.isEmpty())
        return result;

    const QString upper = trimmed.toUpper().replace(' ', "");
    if (upper == "MOD256") {
        result.kind = Spec::Mod256;
        result.width = 8;
        result.valid = true;
        return result;
    }
    if (upper == "XOR") {
        result.kind = Spec::Xor;
        result.width = 8;
        result.valid = true;
        return result;
    }
    if (upper == "MOD65536") {
        result.kind = Spec::Mod65536;
        result.width = 16;
        result.valid = true;
        return result;
    }
    if (upper == "LRC" || upper == "-MOD256") {
        result.kind = Spec::Lrc;
        result.width = 8;
        result.valid = true;
        return result;
    }
    if (upper == "LRC-ASCII") {
        result.kind = Spec::LrcAscii;
        result.width = 8;
        result.valid = true;
        return result;
    }

    // Predefined CRCs, given as their generic definitions.
    struct Named { const char *name; int width; quint32 poly, init, xout; bool ri, ro; };
    static const Named named[] = {
        {"CRC-7",      7,  0x09,      0x00,      0x00,      false, false},
        {"CRC-8",      8,  0x07,      0x00,      0x00,      false, false},
        {"CRC-DOW",    8,  0x31,      0x00,      0x00,      true,  true},
        {"CRC-CCITT", 16,  0x1021,    0xFFFF,    0x0000,    false, false},
        {"CRC-XMODEM",16,  0x1021,    0x0000,    0x0000,    false, false},
        {"CRC-16",    16,  0x8005,    0x0000,    0x0000,    true,  true},
        {"CRC-MODBUS",16,  0x8005,    0xFFFF,    0x0000,    true,  true},
        {"CRC-32",    32,  0x04C11DB7,0xFFFFFFFF,0xFFFFFFFF,true,  true},
    };
    for (const Named &n : named) {
        if (upper == n.name) {
            result.kind = Spec::Crc;
            result.width = n.width;
            result.polynomial = n.poly;
            result.init = n.init;
            result.finalXor = n.xout;
            result.reflectIn = n.ri;
            result.reflectOut = n.ro;
            result.valid = true;
            return result;
        }
    }

    // Generic form: CRC:width,polynomial,init,finalXor,reflectedIn,reflectedOut
    if (upper.startsWith("CRC:")) {
        const QStringList parts = trimmed.mid(4).split(',');
        if (parts.size() == 6) {
            bool okW = false, okP = false, okI = false, okX = false;
            const int width = parts.at(0).toInt(&okW);
            const quint32 poly = parts.at(1).toUInt(&okP, 16);
            const quint32 init = parts.at(2).toUInt(&okI, 16);
            const quint32 xout = parts.at(3).toUInt(&okX, 16);
            const bool ri = parts.at(4).trimmed().toUpper() == "YES";
            const bool ro = parts.at(5).trimmed().toUpper() == "YES";
            if (okW && okP && okI && okX && width >= 1 && width <= 32) {
                result.kind = Spec::Crc;
                result.width = width;
                result.polynomial = poly;
                result.init = init;
                result.finalXor = xout;
                result.reflectIn = ri;
                result.reflectOut = ro;
                result.valid = true;
                return result;
            }
        }
    }
    return result;
}

QByteArray Checksum::compute(const Spec &spec, const QByteArray &data) {
    if (!spec.valid)
        return QByteArray();
    quint32 value = 0;
    switch (spec.kind) {
    case Spec::Mod256:
        for (uchar byte : data)
            value = (value + byte) & 0xFF;
        return QByteArray(1, char(value));
    case Spec::Xor:
        for (uchar byte : data)
            value ^= byte;
        return QByteArray(1, char(value & 0xFF));
    case Spec::Mod65536:
        for (uchar byte : data)
            value = (value + byte) & 0xFFFF;
        return QByteArray{char((value >> 8) & 0xFF), char(value & 0xFF)};
    case Spec::Lrc:
        for (uchar byte : data)
            value = (value + byte) & 0xFF;
        return QByteArray(1, char((-value) & 0xFF));
    case Spec::LrcAscii: {
        // The input is a readable hex text ("31313033..."); sum the decoded
        // bytes and return the two's complement as two ASCII hex characters.
        const QString hex = QString::fromLatin1(data)
                                .remove(QRegularExpression(QStringLiteral("[^0-9A-Fa-f]")));
        QByteArray decoded;
        if (hex.size() % 2 == 0) {
            for (int i = 0; i < hex.size(); i += 2)
                decoded.append(char(hex.mid(i, 2).toInt(nullptr, 16)));
        } else {
            decoded = data;
        }
        for (uchar byte : decoded)
            value = (value + byte) & 0xFF;
        const int lrc = (-value) & 0xFF;
        return QString::asprintf("%02X", lrc).toLatin1();
    }
    case Spec::Crc: {
        value = spec.init;
        const int width = spec.width;
        const quint32 top = 1u << (width - 1);
        const quint32 mask = width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1);
        for (uchar byte : data) {
            const quint32 in = spec.reflectIn ? reflect(byte, 8) : quint32(byte);
            value ^= in << (width - 8);
            for (int bit = 0; bit < 8; ++bit) {
                if (value & top)
                    value = ((value << 1) ^ spec.polynomial) & mask;
                else
                    value = (value << 1) & mask;
            }
        }
        if (spec.reflectOut)
            value = reflect(value, width);
        value ^= spec.finalXor;
        value &= mask;
        const int size = (width + 7) / 8;
        QByteArray out;
        for (int i = 0; i < size; ++i)
            out.append(char((value >> (8 * (size - 1 - i))) & 0xFF));
        return out;
    }
    }
    return QByteArray();
}

QByteArray Checksum::compute(const QString &spec, const QByteArray &data) {
    return compute(parse(spec), data);
}

Checksum::Area Checksum::parseArea(const QString &areaText) {
    Area area;
    QString text = areaText.trimmed();
    if (text.isEmpty()) {
        area.error = QObject::tr("Empty checksum specification");
        return area;
    }

    // Strip a trailing "# comment".
    const int hash = text.indexOf('#');
    if (hash >= 0) {
        area.comment = text.mid(hash + 1).trimmed();
        text = text.left(hash).trimmed();
    }

    // Leading "(start,len)" area definition.
    static const QRegularExpression areaRe(
        QStringLiteral("^\\(\\s*(-?\\d+)\\s*,\\s*(-?\\d+|len)\\s*\\)\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    auto areaMatch = areaRe.match(text);
    if (areaMatch.hasMatch()) {
        area.startPos = areaMatch.captured(1).toInt();
        QString lenText = areaMatch.captured(2);
        if (lenText.compare("len", Qt::CaseInsensitive) == 0)
            area.length = -1;   // "len": until the end of the sequence
        else
            area.length = lenText.toInt();
        text = text.mid(areaMatch.capturedLength());
    }

    // The algorithm name may itself contain the letters A and L ("LRC-ASCII"),
    // so tokens are matched against the known algorithm names first; anything
    // left over is a modifier: "A", "L" or "@ position".
    const QStringList tokens = text.split(' ', Qt::SkipEmptyParts);
    QString specText;
    int consumed = 0;
    for (int i = 0; i < tokens.size(); ++i) {
        QString candidate = tokens.at(i);
        // A generic CRC spec must not have been split by spaces; re-join.
        while (i + 1 < tokens.size()
               && !parse(candidate).valid
               && (candidate.toUpper().startsWith("CRC")
                   || candidate.startsWith('('))) {
            ++i;
            candidate += tokens.at(i);
        }
        if (parse(candidate).valid) {
            specText = candidate;
            consumed = i + 1;
            break;
        }
    }
    if (specText.isEmpty()) {
        area.error = QObject::tr("Unknown checksum algorithm in '%1'").arg(text);
        return area;
    }
    area.spec = parse(specText);
    if (!area.spec.valid) {
        area.error = QObject::tr("Unknown checksum algorithm '%1'").arg(specText);
        return area;
    }

    for (int i = consumed; i < tokens.size(); ++i) {
        QString token = tokens.at(i);
        if (token.startsWith('@')) {
            token = token.mid(1);
            if (token.isEmpty() && i + 1 < tokens.size())
                token = tokens.at(++i);
        }
        if (token.compare("A", Qt::CaseInsensitive) == 0)
            area.asciiResult = true;
        else if (token.compare("L", Qt::CaseInsensitive) == 0)
            area.littleEndian = true;
        else if (token.contains(QRegularExpression(QStringLiteral("^[+-]?\\d+$"))))
            area.targetPos = token.toInt();
        else {
            area.error = QObject::tr("Unexpected checksum option '%1'").arg(token);
            return area;
        }
    }
    area.ok = true;
    return area;
}

namespace {

// Resolves a 1-based Docklight character position against a sequence size.
// Negative values count from the end (-1 = last character). Returns an
// index into the sequence, or -1 when out of range.
int resolvePos(int pos, int size, bool oneBased) {
    int index = pos;
    if (oneBased)
        index = pos > 0 ? pos - 1 : pos;   // negative stay as-is, from end
    if (index < 0)
        index += size;
    if (index < 0 || index >= size)
        return -1;
    return index;
}

}  // namespace

bool Checksum::applyToSequence(const Area &area, QByteArray &sequence, QString *error) {
    if (!area.ok) {
        if (error)
            *error = area.error;
        return false;
    }
    const int resultSize = specBytes(area.spec);

    // Default calculation area: everything before the result area.
    int startPos = area.startPos;
    int endPos = area.length;
    if (startPos == 0 && endPos == 0) {
        // Docklight default: from the first character, everything before the
        // checksum result area.
        const int target = area.targetPos == 0
                               ? sequence.size() - (area.asciiResult ? 3 : resultSize)
                               : (area.targetPos > 0 ? area.targetPos - 1
                                                     : sequence.size() + area.targetPos);
        startPos = 1;
        endPos = target;   // exclusive end, as character count
    } else if (area.startPos != 0 && area.length == 0) {
        endPos = sequence.size() + 1;   // until the last character
    }
    if (area.length == -1)
        endPos = sequence.size() + 1;

    const int start = resolvePos(startPos, sequence.size(), true);
    int dataEnd;
    if (endPos < 0)
        dataEnd = sequence.size() + endPos + 1;   // -1 = include the last char
    else
        dataEnd = endPos;                          // character count from start
    if (start < 0 || dataEnd <= start || dataEnd > sequence.size()) {
        if (error)
            *error = QObject::tr("Checksum area does not fit the sequence");
        return false;
    }
    QByteArray computed = compute(area.spec, sequence.mid(start, dataEnd - start));
    if (computed.isEmpty()) {
        if (error)
            *error = QObject::tr("Checksum computation failed");
        return false;
    }
    // LRC-ASCII results are already readable hex characters.
    if (area.asciiResult && area.spec.kind != Spec::LrcAscii)
        computed = asciiHex(computed, area.littleEndian);
    else if (area.littleEndian && area.spec.kind != Spec::LrcAscii)
        std::reverse(computed.begin(), computed.end());

    // Target position: default last bytes (or one before the end for ASCII
    // results, leaving room for a trailing CR).
    int target = area.targetPos;
    if (target == 0)
        target = -(computed.size() + (area.asciiResult ? 2 : 0));
    int index = target > 0 ? target - 1 : sequence.size() + target;
    if (index < 0 || index + computed.size() > sequence.size()) {
        if (error)
            *error = QObject::tr("Checksum result does not fit the sequence");
        return false;
    }
    sequence.replace(index, computed.size(), computed);
    return true;
}

bool Checksum::validateSequence(const Area &area, const QByteArray &sequence, QString *error) {
    if (!area.ok) {
        if (error)
            *error = area.error;
        return false;
    }
    // Compute what the checksum should be over the protected area, then
    // compare it with the bytes stored in the sequence.
    const int resultSize = specBytes(area.spec);
    int startPos = area.startPos;
    int endPos = area.length;
    if (startPos == 0 && endPos == 0) {
        const int target = area.targetPos == 0
                               ? sequence.size() - (area.asciiResult ? 3 : resultSize)
                               : (area.targetPos > 0 ? area.targetPos - 1
                                                     : sequence.size() + area.targetPos);
        startPos = 1;
        endPos = target;
    } else if (area.startPos != 0 && area.length == 0) {
        endPos = sequence.size() + 1;
    }
    if (area.length == -1)
        endPos = sequence.size() + 1;

    const int start = resolvePos(startPos, sequence.size(), true);
    int dataEnd = endPos < 0 ? sequence.size() + endPos + 1 : endPos;
    if (start < 0 || dataEnd <= start || dataEnd > sequence.size()) {
        if (error)
            *error = QObject::tr("Checksum area does not fit the sequence");
        return false;
    }
    QByteArray expected = compute(area.spec, sequence.mid(start, dataEnd - start));
    if (expected.isEmpty()) {
        if (error)
            *error = QObject::tr("Checksum computation failed");
        return false;
    }
    if (area.asciiResult && area.spec.kind != Spec::LrcAscii) {
        expected = asciiHex(expected, area.littleEndian);
    } else if (area.littleEndian && area.spec.kind != Spec::LrcAscii) {
        std::reverse(expected.begin(), expected.end());
    }

    int target = area.targetPos;
    if (target == 0)
        target = -(expected.size() + (area.asciiResult ? 2 : 0));
    const int index = target > 0 ? target - 1 : sequence.size() + target;
    if (index < 0 || index + expected.size() > sequence.size()) {
        if (error)
            *error = QObject::tr("Checksum result does not fit the sequence");
        return false;
    }
    const QByteArray stored = sequence.mid(index, expected.size());
    return stored == expected;
}

QByteArray Checksum::toAsciiHex(const QByteArray &bytes, bool littleEndian) {
    return asciiHex(bytes, littleEndian);
}
