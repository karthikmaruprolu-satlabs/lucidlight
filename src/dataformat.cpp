#include "dataformat.h"

#include <QRegularExpression>

ParsedBytes DataFormat::parse(const QString &input, Format format) {
    ParsedBytes result;
    if (format == Format::Text) {
        for (int i = 0; i < input.size(); ++i) {
            const QChar c = input.at(i);
            if (c == QLatin1Char('[')) {
                if (i + 1 < input.size() && input.at(i + 1) == QLatin1Char('[')) {
                    result.bytes.append('[');
                    result.wildcard.append(false);
                    ++i;
                    continue;
                }
                // [0D] inserts a hex byte; 1 or 2 hex digits are both accepted.
                int j = i + 1;
                while (j < input.size() && j <= i + 2 && isxdigit(input.at(j).toLatin1()))
                    ++j;
                const int digits = j - (i + 1);
                if (digits == 0 || j >= input.size() || input.at(j) != QLatin1Char(']')) {
                    result.error = QObject::tr("Invalid escape at position %1").arg(i + 1);
                    return result;
                }
                result.bytes.append(char(input.mid(i + 1, digits).toInt(nullptr, 16)));
                // One byte came out of the escape, so one mask entry.
                result.wildcard.append(false);
                i = j;
                continue;
            }
            if (c == QLatin1Char('?') && i + 1 < input.size() && input.at(i + 1) == QLatin1Char('?')) {
                result.bytes.append(char(0));
                result.wildcard.append(true);
                ++i;
                continue;
            }
            const QString segment = QString(c);
            const QByteArray encoded = segment.toUtf8();
            result.bytes.append(encoded);
            for (int k = 0; k < encoded.size(); ++k)
                result.wildcard.append(false);
        }
        return result;
    }

    if (format == Format::Hex) {
        QString compact;
        compact.reserve(input.size());
        for (const QChar c : input)
            if (c != QLatin1Char(' ') && c != QLatin1Char('\t') && c != QLatin1Char(',')
                && c != QLatin1Char(':') && c != QLatin1Char('\n'))
                compact.append(c);
        if (compact.size() % 2 != 0) {
            result.error = QObject::tr("Hex input needs pairs of digits");
            return result;
        }
        for (int i = 0; i < compact.size(); i += 2) {
            const QString pair = compact.mid(i, 2);
            if (pair == QLatin1String("??")) {
                result.bytes.append(char(0));
                result.wildcard.append(true);
                continue;
            }
            bool ok = false;
            const int value = pair.toInt(&ok, 16);
            if (!ok) {
                result.error = QObject::tr("'%1' is not a hex byte").arg(pair);
                return result;
            }
            result.bytes.append(char(value));
            result.wildcard.append(false);
        }
        return result;
    }

    // Dec mode: whitespace-separated decimal byte values.
    const QStringList tokens = input.split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (token == QLatin1String("??")) {
            result.bytes.append(char(0));
            result.wildcard.append(true);
            continue;
        }
        bool ok = false;
        const int value = token.toInt(&ok, 10);
        if (!ok || value < 0 || value > 255) {
            result.error = QObject::tr("'%1' is not a decimal byte").arg(token);
            return result;
        }
        result.bytes.append(char(value));
        result.wildcard.append(false);
    }
    return result;
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
    QString dec;
    for (int i = 0; i < data.size(); ++i) {
        if (i)
            dec.append(QLatin1Char(' '));
        dec.append(QString::number(uchar(data.at(i))));
    }
    return dec;
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
