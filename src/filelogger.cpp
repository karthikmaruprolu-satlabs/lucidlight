#include "filelogger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

FileLogger::FileLogger(QObject *parent) : QObject(parent) {}

FileLogger::~FileLogger() {
    stop();
}

bool FileLogger::start(const QString &baseFilePath, bool appendData,
                       const QString &representations, int format, bool noHeaders) {
    stop();
    QString base = baseFilePath.trimmed();
    if (base.isEmpty())
        return false;

    m_representations.clear();
    for (const QChar c : representations.toUpper())
        if (QStringLiteral("AHDB").contains(c) && !m_representations.contains(c))
            m_representations.append(c);
    if (m_representations.isEmpty())
        m_representations.append("H");
    m_format = qBound(0, format, 2);

    static const char *suffixes[] = {"_asc", "_hex", "_dec", "_bin"};
    static const char *reps[] = {"A", "H", "D", "B"};
    QDir().mkpath(QFileInfo(base).absolutePath());
    for (int r = 0; r < 4; ++r) {
        if (!m_representations.contains(QLatin1Char(*reps[r])))
            continue;
        const QString suffix = m_format == 1 ? ".htm" : m_format == 2 ? ".rtf" : ".txt";
        File f;
        f.file = new QFile(base + suffixes[r] + suffix, this);
        if (!f.file->open(QIODevice::WriteOnly | (appendData ? QIODevice::Append
                                                             : QIODevice::Truncate))) {
            delete f.file;
            continue;
        }
        f.stream = new QTextStream(f.file);
        m_files.append(f);
        if (!noHeaders)
            writeHeader(m_files.last(), QLatin1String(reps[r]));
    }
    m_active = !m_files.isEmpty();
    return m_active;
}

void FileLogger::stop() {
    if (m_active) {
        for (File &f : m_files) {
            writeFooter(f);
            f.stream->flush();
            delete f.stream;
            delete f.file;
        }
    } else {
        for (File &f : m_files) {
            delete f.stream;
            delete f.file;
        }
    }
    m_files.clear();
    m_active = false;
}

void FileLogger::onRow(int direction, int channel, const QByteArray &data,
                       const QString &comment, qint64 stampMs, bool highlighted) {
    if (!m_active)
        return;
    static const char *reps[] = {"A", "H", "D", "B"};
    for (int i = 0; i < m_files.size(); ++i)
        writeRow(m_files[i], QLatin1String(reps[i]), direction, channel, data,
                 comment, stampMs, highlighted);
}

QString FileLogger::asciiWithControlNames(const QByteArray &data) {
    static const char *names[] = {"NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL",
                                  "BS",  "HT",  "LF",  "VT",  "FF",  "CR",  "SO",  "SI",
                                  "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB",
                                  "CAN", "EM",  "SUB", "ESC", "FS",  "GS",  "RS",  "US"};
    QString out;
    for (uchar byte : data) {
        if (byte >= 0x20 && byte < 0x7F)
            out += QChar(byte);
        else if (byte < 0x20)
            out += QString("<%1>").arg(names[byte]);
        else if (byte == 0x7F)
            out += "<DEL>";
        else
            out += QString("<%1>").arg(byte, 3, 10, QChar('0'));
    }
    return out;
}

QString FileLogger::decimalText(const QByteArray &data) {
    QString out;
    for (int i = 0; i < data.size(); ++i) {
        if (i)
            out += QLatin1Char(' ');
        out += QString::number(uchar(data.at(i)));
    }
    return out;
}

QString FileLogger::binaryText(const QByteArray &data) {
    QString out;
    for (int i = 0; i < data.size(); ++i) {
        if (i)
            out += QLatin1Char(' ');
        out += QString("%1").arg(uchar(data.at(i)), 8, 2, QChar('0'));
    }
    return out;
}

QString FileLogger::stampText(qint64 stampMs) {
    return QDateTime::fromMSecsSinceEpoch(stampMs).toString("MM/dd/yyyy HH:mm:ss.zzz");
}

void FileLogger::writeHeader(File &f, const QString &representation) {
    *f.stream << (m_format == 0 ? "" : m_format == 1 ? "<pre>\n" : "{\\rtf1\n");
    *f.stream << "Docklight Log File started... (" << stampText(QDateTime::currentMSecsSinceEpoch())
              << ", " << representation << ")\n";
}

void FileLogger::writeFooter(File &f) {
    *f.stream << "Docklight Log File stopped (" << stampText(QDateTime::currentMSecsSinceEpoch())
              << ")\n";
    if (m_format == 1)
        *f.stream << "</pre>\n";
    else if (m_format == 2)
        *f.stream << "}\n";
}

void FileLogger::writeRow(File &f, const QString &representation, int direction, int channel,
                          const QByteArray &data, const QString &comment, qint64 stampMs,
                          bool highlighted) {
    QString body;
    QString tag;
    if (direction == CommLog::Comment || direction == CommLog::Event) {
        body = comment;
        tag = direction == CommLog::Comment ? QString() : QStringLiteral("NOTE");
    } else {
        if (representation == QLatin1String("A"))
            body = asciiWithControlNames(data);
        else if (representation == QLatin1String("H"))
            body = DataFormat::toHex(data);
        else if (representation == QLatin1String("D"))
            body = decimalText(data);
        else
            body = binaryText(data);
        tag = direction == CommLog::Tx ? QStringLiteral("TX") : QStringLiteral("RX");
        if (channel >= 0)
            tag += QString::number(channel + 1);
    }
    QString prefix = stampText(stampMs) + QLatin1Char(' ');
    if (!tag.isEmpty())
        prefix += QLatin1Char('[') + tag + QStringLiteral("] - ");
    if (m_format == 1) {
        const char *color = direction == CommLog::Tx ? "#2980b9"
                            : direction == CommLog::Rx ? "#27ae60"
                            : highlighted ? "#c0392b" : "#888888";
        *f.stream << "<span style=\"color:" << color << "\">" << prefix << body << "</span>\n";
    } else if (m_format == 2) {
        *f.stream << prefix << body << "\\par\n";
    } else {
        *f.stream << prefix << body << "\n";
    }
    f.stream->flush();
}
