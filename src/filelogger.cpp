#include "filelogger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>
static qint64 nowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return qint64(ts.tv_sec) * 1000000 + qint64(ts.tv_nsec) / 1000;
}

static QString stampTextUs(qint64 us) {
    return QString::number(us / 1000000.0, 'f', 6);
}


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
    m_format = qBound(0, format, 4);

    static const char *suffixes[] = {"_asc", "_hex", "_dec", "_bin"};
    static const char *reps[] = {"A", "H", "D", "B"};
    QDir().mkpath(QFileInfo(base).absolutePath());
    for (int r = 0; r < 4; ++r) {
        if (!m_representations.contains(QLatin1Char(*reps[r])))
            continue;
        const QString suffix = m_format == 1 ? ".htm"
                               : m_format == 2 ? ".rtf"
                               : m_format == 3 ? ".md"
                               : m_format == 4 ? ".csv"
                               : ".txt";
        File f;
        f.file = new QFile(base + suffixes[r] + suffix, this);
        if (!f.file->open(QIODevice::WriteOnly | (appendData ? QIODevice::Append
                                                             : QIODevice::Truncate))) {
            delete f.file;
            continue;
        }
        f.stream = new QTextStream(f.file);
        f.representation = QLatin1Char(*reps[r]);
        m_files.append(f);
        if (!noHeaders)
            writeHeader(m_files.last(), QLatin1String(reps[r]));
    }
    m_startUs = nowUs();
    m_active = !m_files.isEmpty();
    return m_active;
}


bool FileLogger::writeSnapshot(const QVector<CommLog::Row> &rows,
                               const QString &path, int format,
                               QChar representation) {
    const int savedFormat = m_format;
    m_format = format;
    File f;
    f.file = new QFile(path, this);
    f.representation = QString(representation);
    const bool ok = f.file->open(QIODevice::WriteOnly | QIODevice::Text);
    if (!ok) {
        delete f.file;
        m_format = savedFormat;
        return false;
    }
    f.stream = new QTextStream(f.file);
    m_files.append(f);
    m_active = true;
    writeHeader(m_files.last(), representation);
    const qint64 baseUs = rows.isEmpty() ? 0 : rows.first().stampUs;
    for (const auto &row : rows) {
        m_files.last().stampUs = row.stampUs - baseUs;
        writeRow(m_files.last(), representation, row.direction, row.channel,
                 row.data, row.text, row.stampMs, row.highlighted);
    }
    writeFooter(m_files.last());
    f.stream->flush();
    delete f.stream;
    f.file->deleteLater();
    m_files.clear();
    m_active = false;
    m_format = savedFormat;
    return true;
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
    for (File &f : m_files)
        writeRow(f, f.representation, direction, channel, data, comment,
                 stampMs, highlighted);
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

// Markdown table cell text: escape pipes and backticks so a data row
// cannot break the table.
QString FileLogger::markdownCell(const QString &text) {
    QString out = text;
    out.replace('\\', "\\\\");
    out.replace('|', "\\|");
    out.replace('`', "\\`");
    return out;
}

QString FileLogger::stampText(qint64 stampMs) {
    return QDateTime::fromMSecsSinceEpoch(stampMs).toString("MM/dd/yyyy HH:mm:ss.zzz");
}

// Seconds-only stamp with microsecond precision.
void FileLogger::writeHeader(File &f, const QString &representation) {
    if (m_format == 1) {
        // Docklight "Customize HTML output": the CSS font/style of the log
        // comes from the customizable template.
        const QString css = m_htmlStyle.isEmpty()
            ? QStringLiteral(
                  "font-family:'JetBrains Mono','Courier New',monospace; "
                  "font-size:small")
            : m_htmlStyle;
        *f.stream << "<!DOCTYPE html>\n<html>\n<head>\n"
                  << "<meta charset=\"utf-8\">\n"
                  << "<title>Docklight Log File</title>\n"
                  << "<style>\nbody { background:#ffffff; }\n"
                  << "pre { " << css << "; }\n"
                  << "</style>\n</head>\n<body>\n<pre>\n";
    } else if (m_format == 2) {
        *f.stream << "{\\rtf1\n";
    }
    if (m_format == 3)
        *f.stream << "# Lucidlight communication log\n\n";
    else if (m_format == 4)
        *f.stream << "time,direction,channel,data_hex,data_ascii\n";
    if (m_format != 4) {
        // CSV keeps the file strictly tabular — no prose lines.
        *f.stream << "Docklight Log File started... ("
                  << stampText(QDateTime::currentMSecsSinceEpoch())
                  << ", " << representation << ")\n";
        if (m_format == 3)
            *f.stream << "\n";
    }
}

void FileLogger::writeFooter(File &f) {
    if (m_format != 4)
        *f.stream << "Docklight Log File stopped ("
                  << stampText(QDateTime::currentMSecsSinceEpoch()) << ")\n";
    if (m_format == 1)
        *f.stream << "</pre>\n</body>\n</html>\n";
    else if (m_format == 2)
        *f.stream << "}\n";
    else if (m_format == 3)
        *f.stream << "\n*Docklight Log File stopped*\n";
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
    if (m_format == 4) {
        // CSV row: RFC 4180 quoting, hex and ASCII data side by side so
        // spreadsheets and analysis tools can consume a session directly.
        const QString stamp = m_microStamps
            ? stampTextUs(f.stampUs >= 0 ? f.stampUs : nowUs() - m_startUs)
            : stampText(stampMs);
        const QString dirCell = tag == "NOTE" ? "NOTE"
                                : tag.isEmpty() ? "Comment" : tag;
        const QString chanCell = (direction == CommLog::Tx || direction == CommLog::Rx)
                                     ? QString::number(channel + 1)
                                     : QString();
        auto quote = [](QString s) {
            s.replace('"', "\"\"");
            return "\"" + s + "\"";
        };
        const QString hexCell = (direction == CommLog::Tx || direction == CommLog::Rx)
                                    ? DataFormat::toHex(data)
                                    : QString();
        const QString asciiCell = (direction == CommLog::Tx || direction == CommLog::Rx)
                                      ? asciiWithControlNames(data)
                                      : comment;
        *f.stream << quote(stamp) << ',' << quote(dirCell) << ','
                  << quote(chanCell) << ',' << quote(hexCell) << ','
                  << quote(asciiCell) << "\n";
        f.stream->flush();
        return;
    }
    if (m_format == 3) {
        // Markdown table row. The first data row also emits the table head.
        if (!f.mdHeadDone) {
            *f.stream << "| Stamp | Direction | Data (" << representation
                      << ") |\n|---|---|---|\n";
            f.mdHeadDone = true;
        }
        const QString stamp = m_microStamps
            ? stampTextUs(f.stampUs >= 0 ? f.stampUs : nowUs() - m_startUs)
            : stampText(stampMs);
        QString dirCell;
        if (tag == "NOTE")
            dirCell = "NOTE";
        else if (tag.isEmpty())
            dirCell = "Comment";
        else
            dirCell = tag;
        const QString cell = markdownCell(body);
        *f.stream << "| " << stamp << " | " << dirCell << " | "
                  << (highlighted && direction != CommLog::Comment
                          ? "**" + cell + "**"
                          : "`" + cell + "`")
                  << " |\n";
        f.stream->flush();
        return;
    }
    QString prefix = m_microStamps
        ? stampTextUs(f.stampUs >= 0 ? f.stampUs : nowUs() - m_startUs)
              + QLatin1Char(' ')
        : stampText(stampMs) + QLatin1Char(' ');
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
