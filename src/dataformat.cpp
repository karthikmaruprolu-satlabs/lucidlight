#include "dataformat.h"

#include <QRegularExpression>

namespace {

bool isHexLetter(const QChar &c) {
    return (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
           || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
}

// Parses one bracket token starting at '[', i as the bracket index.
// Returns false when the token is malformed (error already set).
bool parseBracket(const QString &input, int &i, ParsedBytes &result) {
    const int open = i;
    int j = i + 1;
    if (j < input.size() && input.at(j) == QLatin1Char('[')) {
        // "[[" inserts a literal '['.
        ParsedBytes::Element e;
        e.kind = ParsedBytes::Element::Data;
        e.value = '[';
        result.elements.append(e);
        i = j;
        return true;
    }
    static const QString funcChars = QStringLiteral("&%!^");
    if (j >= input.size()
        || !(input.at(j).isLetter() || input.at(j).isDigit()
             || funcChars.contains(input.at(j)))) {
        result.error = QObject::tr("Invalid escape at position %1").arg(open + 1);
        return false;
    }
    // Classic hex byte escape: [0D] or [D].
    if (input.at(j).isDigit()
        || isHexLetter(input.at(j))) {
        int k = j;
        while (k < input.size() && k <= j + 1 && input.at(k).isDigit())
            ++k;
        // Also accept a-f digits in the second position.
        while (k < input.size() && k <= j + 1
               && QStringLiteral("abcdefABCDEF").contains(input.at(k)))
            ++k;
        const int digits = k - j;
        if (digits == 0 || k >= input.size() || input.at(k) != QLatin1Char(']')) {
            result.error = QObject::tr("Invalid escape at position %1").arg(open + 1);
            return false;
        }
        ParsedBytes::Element e;
        e.kind = ParsedBytes::Element::Data;
        e.value = uchar(input.mid(j, digits).toInt(nullptr, 16));
        result.elements.append(e);
        i = k;
        return true;
    }
    const QChar func = input.at(j).toUpper();
    ++j;
    if (func == QLatin1Char('%')) {
        if (j >= input.size() || input.at(j) != QLatin1Char(']')) {
            result.error = QObject::tr("Invalid escape at position %1").arg(open + 1);
            return false;
        }
        ParsedBytes::Element e;
        e.kind = ParsedBytes::Element::Break;
        result.elements.append(e);
        i = j;
        return true;
    }
    // [&NN], [!V] and [^MM VV] read values up to the closing bracket.
    int close = input.indexOf(QLatin1Char(']'), j);
    if (close < 0) {
        result.error = QObject::tr("Invalid escape at position %1").arg(open + 1);
        return false;
    }
    const QString body = input.mid(j, close - j).trimmed();
    ParsedBytes::Element e;
    if (func == QLatin1Char('&')) {
        bool ok = false;
        const int value = body.toInt(&ok);
        if (!ok || value < 0 || value > 255) {
            result.error = QObject::tr("Delay needs a value from 0 to 255");
            return false;
        }
        e.kind = ParsedBytes::Element::Delay;
        e.value = uchar(value);
    } else if (func == QLatin1Char('!')) {
        const QString compact = QString(body).remove(' ');
        bool ok = false;
        const int value = compact.toInt(&ok, 16);
        if (!ok || value < 0 || value > 255) {
            result.error = QObject::tr("Handshake needs a hex value from 00 to FF");
            return false;
        }
        e.kind = ParsedBytes::Element::Handshake;
        e.value = uchar(value);
    } else if (func == QLatin1Char('^')) {
        const QStringList parts = body.split(' ', Qt::SkipEmptyParts);
        if (parts.size() != 2) {
            result.error = QObject::tr("Bitwise comparison needs mask and value");
            return false;
        }
        bool okM = false, okV = false;
        const int mask = parts.at(0).toInt(&okM, 16);
        const int expect = parts.at(1).toInt(&okV, 16);
        if (!okM || !okV || mask < 0 || mask > 255 || expect < 0 || expect > 255) {
            result.error = QObject::tr("Bitwise mask and value must be hex bytes");
            return false;
        }
        e.kind = ParsedBytes::Element::Bitwise;
        e.mask = uchar(mask);
        e.expect = uchar(expect);
    } else {
        result.error = QObject::tr("Unknown function character '%1'").arg(func);
        return false;
    }
    result.elements.append(e);
    i = close;
    return true;
}

}  // namespace

ParsedBytes DataFormat::parse(const QString &input, Format format) {
    ParsedBytes result;
    if (format == Format::Text) {
        for (int i = 0; i < input.size(); ++i) {
            const QChar c = input.at(i);
            if (c == QLatin1Char('[')) {
                if (!parseBracket(input, i, result))
                    return result;
                continue;
            }
            if (c == QLatin1Char('<')) {
                // Docklight-style control character notation: <CR>, <LF>,
                // <NUL> ... <DEL>, <#> / <?> wildcards, or a decimal code
                // like <13>. Anything unrecognized stays literal text.
                const int close = input.indexOf(QLatin1Char('>'), i + 1);
                if (close > i + 1) {
                    const QString name =
                        input.mid(i + 1, close - i - 1).toUpper();
                    bool isControl = false;
                    for (int b = 0; b <= 255 && !isControl; ++b) {
                        const QString n = DataFormat::controlName(uchar(b));
                        if (!n.isEmpty() && n.toUpper() == name)
                            isControl = true;
                    }
                    if (name == QLatin1String("?")) {
                        ParsedBytes::Element e;
                        e.kind = ParsedBytes::Element::WildOne;
                        result.elements.append(e);
                        i = close;
                        continue;
                    }
                    if (name == QLatin1String("#")) {
                        ParsedBytes::Element e;
                        e.kind = ParsedBytes::Element::WildZeroOrOne;
                        result.elements.append(e);
                        i = close;
                        continue;
                    }
                    bool okDec = false;
                    const int code = name.toInt(&okDec, 10);
                    if (okDec && code >= 0 && code <= 255)
                        isControl = true;
                    if (isControl) {
                        uchar value = okDec ? uchar(code) : uchar(0);
                        if (!okDec) {
                            // Resolve the mnemonic back to its byte value.
                            for (int b = 0; b < 256; ++b)
                                if (DataFormat::controlName(uchar(b))
                                        .toUpper() == name)
                                    value = uchar(b);
                        }
                        ParsedBytes::Element e;
                        e.kind = ParsedBytes::Element::Data;
                        e.value = value;
                        result.elements.append(e);
                        i = close;
                        continue;
                    }
                }
            }
            if (c == QLatin1Char('?')) {
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::WildOne;
                result.elements.append(e);
                continue;
            }
            if (c == QLatin1Char('#')) {
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::WildZeroOrOne;
                result.elements.append(e);
                continue;
            }
            const QByteArray encoded = QString(c).toUtf8();
            for (int k = 0; k < encoded.size(); ++k) {
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::Data;
                e.value = uchar(encoded.at(k));
                result.elements.append(e);
            }
        }
        return result;
    }

    if (format == Format::Hex) {
        // Bracket function tokens may contain spaces ([^0F 05]), so tokenize
        // with a regex that keeps them intact.
        static const QRegularExpression tokenRe(
            QStringLiteral("\\[[^\\]]*\\]|[^\\s,:]+"));
        auto it = tokenRe.globalMatch(input);
        while (it.hasNext()) {
            const QString token = it.next().captured(0);
            if (token.startsWith(QLatin1Char('['))) {
                int i = input.indexOf(token);
                if (!parseBracket(input, i, result))
                    return result;
                continue;
            }
            if (token == QLatin1String("?")) {
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::WildOne;
                result.elements.append(e);
                continue;
            }
            if (token == QLatin1String("#")) {
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::WildZeroOrOne;
                result.elements.append(e);
                continue;
            }
            if (token == QLatin1String("??")) {
                // Docklight shows one wildcard byte as "??" in HEX notation.
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::WildOne;
                result.elements.append(e);
                continue;
            }
            if (token.size() % 2 != 0) {
                result.error = QObject::tr("'%1' is not a hex byte").arg(token);
                return result;
            }
            for (int p = 0; p < token.size(); p += 2) {
                bool ok = false;
                const int value = token.mid(p, 2).toInt(&ok, 16);
                if (!ok) {
                    result.error = QObject::tr("'%1' is not a hex byte").arg(token.mid(p, 2));
                    return result;
                }
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::Data;
                e.value = uchar(value);
                result.elements.append(e);
            }
        }
        return result;
    }

    // Binary mode: 8-bit groups, same tokens as decimal.
    if (format == Format::Bin) {
        static const QRegularExpression tokenRe(
            QStringLiteral("\\[[^\\]]*\\]|[^\\s,]+"));
        auto it = tokenRe.globalMatch(input);
        while (it.hasNext()) {
            const QString token = it.next().captured(0);
            if (token.startsWith(QLatin1Char('['))) {
                int i = input.indexOf(token);
                if (!parseBracket(input, i, result))
                    return result;
                continue;
            }
            bool ok = false;
            const int value = token.toInt(&ok, 2);
            if (!ok || value < 0 || value > 255) {
                result.error = QObject::tr("'%1' is not an 8-bit binary group").arg(token);
                return result;
            }
            ParsedBytes::Element e;
            e.kind = ParsedBytes::Element::Data;
            e.value = uchar(value);
            result.elements.append(e);
        }
        return result;
    }

    // Dec mode: decimal byte values, same wildcard and function tokens.
    {
        static const QRegularExpression tokenRe(
            QStringLiteral("\\[[^\\]]*\\]|[^\\s,]+"));
        auto it = tokenRe.globalMatch(input);
        while (it.hasNext()) {
            const QString token = it.next().captured(0);
            if (token.startsWith(QLatin1Char('['))) {
                int i = input.indexOf(token);
                if (!parseBracket(input, i, result))
                    return result;
                continue;
            }
            if (token == QLatin1String("?")) {
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::WildOne;
                result.elements.append(e);
                continue;
            }
            if (token == QLatin1String("#")) {
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::WildZeroOrOne;
                result.elements.append(e);
                continue;
            }
            if (token == QLatin1String("??")) {
                // Docklight shows one wildcard byte as "??" in DEC notation.
                ParsedBytes::Element e;
                e.kind = ParsedBytes::Element::WildOne;
                result.elements.append(e);
                continue;
            }
            bool ok = false;
            const int value = token.toInt(&ok, 10);
            if (!ok || value < 0 || value > 255) {
                result.error = QObject::tr("'%1' is not a decimal byte").arg(token);
                return result;
            }
            ParsedBytes::Element e;
            e.kind = ParsedBytes::Element::Data;
            e.value = uchar(value);
            result.elements.append(e);
        }
    }
    return result;
}

QByteArray ParsedBytes::bytes() const {
    QByteArray out;
    for (const Element &e : elements)
        if (e.kind == Element::Data || e.kind == Element::WildOne
            || e.kind == Element::WildZeroOrOne || e.kind == Element::Bitwise)
            out.append(char(e.kind == Element::Data ? e.value : 0));
    return out;
}

QVector<bool> ParsedBytes::wildcard() const {
    QVector<bool> out;
    for (const Element &e : elements)
        if (e.kind == Element::Data || e.kind == Element::WildOne
            || e.kind == Element::WildZeroOrOne || e.kind == Element::Bitwise)
            out.append(e.kind != Element::Data);
    return out;
}

QString DataFormat::display(const QByteArray &data, Format format) {
    if (format == Format::Text) {
        QString text;
        text.reserve(data.size());
        for (int i = 0; i < data.size(); ++i) {
            const uchar byte = uchar(data.at(i));
            if (byte >= 0x20 && byte < 0x7F)
                text.append(QChar(byte));
            else if (byte == '\t')
                text.append(QLatin1Char('\t'));
            else
                text.append(QChar(0xB7));   // middot for control bytes
        }
        return text;
    }
    if (format == Format::Hex)
        return toHex(data);
    if (format == Format::Bin) {
        // Docklight's Binary view: eight bits per byte, space separated.
        QString bin;
        for (int i = 0; i < data.size(); ++i) {
            if (i)
                bin.append(QLatin1Char(' '));
            bin += QString("%1").arg(uchar(data.at(i)), 8, 2, QChar('0'));
        }
        return bin;
    }
    QString dec;
    for (int i = 0; i < data.size(); ++i) {
        if (i)
            dec.append(QLatin1Char(' '));
        dec.append(QString::number(uchar(data.at(i))));
    }
    return dec;
}

// Docklight's "control characters" view option: bytes below 0x20 (plus DEL)
// show as their mnemonic inside angle brackets.
QString DataFormat::controlName(uchar byte) {
    switch (byte) {
    case 0x00: return "NUL";
    case 0x01: return "SOH";
    case 0x02: return "STX";
    case 0x03: return "ETX";
    case 0x04: return "EOT";
    case 0x05: return "ENQ";
    case 0x06: return "ACK";
    case 0x07: return "BEL";
    case 0x08: return "BS";
    case 0x09: return "TAB";
    case 0x0A: return "LF";
    case 0x0B: return "VT";
    case 0x0C: return "FF";
    case 0x0D: return "CR";
    case 0x0E: return "SO";
    case 0x0F: return "SI";
    case 0x10: return "DLE";
    case 0x11: return "DC1";
    case 0x12: return "DC2";
    case 0x13: return "DC3";
    case 0x14: return "DC4";
    case 0x15: return "NAK";
    case 0x16: return "SYN";
    case 0x17: return "ETB";
    case 0x18: return "CAN";
    case 0x19: return "EM";
    case 0x1A: return "SUB";
    case 0x1B: return "ESC";
    case 0x1C: return "FS";
    case 0x1D: return "GS";
    case 0x1E: return "RS";
    case 0x1F: return "US";
    case 0x7F: return "DEL";
    default: return QString();
    }
}

QString DataFormat::displayWithControlNames(const QByteArray &data) {
    QString text;
    for (int i = 0; i < data.size(); ++i) {
        const uchar byte = uchar(data.at(i));
        if (byte == '\r' && i + 1 < data.size() && data.at(i + 1) == '\n') {
            text += "<CR><LF>";
            ++i;
            continue;
        }
        const QString name = controlName(byte);
        if (!name.isEmpty())
            text += "<" + name + ">";
        else
            text.append(QChar(byte));
    }
    return text;
}

QString DataFormat::toHex(const QByteArray &data) {
    QString hex;
    hex.reserve(data.size() * 3);
    for (int i = 0; i < data.size(); ++i) {
        if (i)
            hex.append(QLatin1Char(' '));
        hex.append(QString::asprintf("%02X", uchar(data.at(i))));
    }
    return hex;
}

QByteArray DataFormat::fromHex(const QString &hex, QString *error) {
    QByteArray bytes;
    QString compact;
    for (const QChar c : hex)
        if (c.isDigit() || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            compact.append(c);
    if (compact.size() % 2 != 0) {
        if (error)
            *error = QObject::tr("Hex input needs pairs of digits");
        return bytes;
    }
    for (int i = 0; i < compact.size(); i += 2)
        bytes.append(char(compact.mid(i, 2).toInt(nullptr, 16)));
    return bytes;
}

QString DataFormat::toTextNotation(const QByteArray &data) {
    QString text;
    for (int i = 0; i < data.size(); ++i) {
        const uchar byte = uchar(data.at(i));
        if (byte == '[')
            text.append(QLatin1String("[["));
        else if (byte >= 0x20 && byte < 0x7F)
            text.append(QChar(byte));
        else
            text.append(QString::asprintf("[%02X]", byte));
    }
    return text;
}
