#pragma once

#include <QByteArray>
#include <QObject>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QVector>

#include "commlog.h"

// Writes Docklight-style log files while communication runs. One file per
// requested representation is created: <base>_asc.txt, <base>_hex.txt,
// <base>_dec.txt and <base>_bin.txt, in plain text, HTML or RTF format.
// This is the machinery behind Start/Stop Communication Logging and the
// DL.StartLogging / DL.StopLogging script commands.
class FileLogger : public QObject {
    Q_OBJECT
public:
    explicit FileLogger(QObject *parent = nullptr);
    ~FileLogger() override;

    // Opens the log files. Returns false when none could be created.
    bool start(const QString &baseFilePath, bool appendData,
               const QString &representations, int format, bool noHeaders);
    void stop();
    bool active() const { return m_active; }
    // Docklight "Customize HTML output": the CSS style applied to the <pre>
    // block of .htm log files. Empty resets to the built-in default.
    void setHtmlStyle(const QString &css) { m_htmlStyle = css; }
    // Seconds-only stamps with 6 decimals (us) in the log files, matching
    // the comm window's microsecond mode. Default: Docklight date stamps.
    void setMicroStamps(bool on) { m_microStamps = on; }
    QString htmlStyle() const { return m_htmlStyle; }
    // Saves a single snapshot file (.htm/.rtf/.txt) replaying buffered rows,
    // for the GUI "Save communication log" command. representation is one of
    // "A", "H", "D", "B"; format 0 text, 1 html, 2 rtf.
    bool writeSnapshot(const QVector<CommLog::Row> &rows, const QString &path,
                       int format, QChar representation);

public slots:
    // Appends one communication row to every open file.
    void onRow(int direction, int channel, const QByteArray &data,
               const QString &comment, qint64 stampMs, bool highlighted);

private:
    struct File {
        QFile *file = nullptr;
        QTextStream *stream = nullptr;
        QString representation;   // "A", "H", "D" or "B"
        qint64 stampUs = -1;      // fixed us offset (snapshot replay); -1 = live clock
        bool mdHeadDone = false;  // markdown table head emitted yet?
    };
    void writeHeader(File &f, const QString &representation);
    void writeRow(File &f, const QString &representation, int direction, int channel,
                  const QByteArray &data, const QString &comment, qint64 stampMs,
                  bool highlighted);
    void writeFooter(File &f);
    static QString asciiWithControlNames(const QByteArray &data);
    static QString decimalText(const QByteArray &data);
    static QString binaryText(const QByteArray &data);
    static QString stampText(qint64 stampMs);
    static QString markdownCell(const QString &text);

    QVector<File> m_files;
    QStringList m_representations;
    int m_format = 0;   // 0 txt, 1 htm, 2 rtf, 3 markdown, 4 csv
    bool m_active = false;
    QString m_htmlStyle;
    bool m_microStamps = false;
    qint64 m_startUs = 0;
};
