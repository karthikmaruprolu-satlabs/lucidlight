#pragma once

#include <QByteArray>
#include <QObject>
#include <QVector>

// Watches the incoming byte stream for receive-sequence patterns and fires
// the matching trigger. Patterns may contain ?? wildcards (one unknown byte
// each). A match consumes the stream up to its end, so a pattern that appears
// twice in one burst fires twice.
class SequenceMatcher : public QObject {
    Q_OBJECT
public:
    struct Pattern {
        int sequenceId = 0;
        QByteArray bytes;
        QVector<bool> wildcard;
    };

    explicit SequenceMatcher(QObject *parent = nullptr);

    void setPatterns(const QVector<Pattern> &patterns);
    void reset();
    void feed(const QByteArray &data);

signals:
    // Emitted once per recognized occurrence, with the matched bytes.
    void matched(int sequenceId, const QByteArray &bytes);

private:
    void scan();

    QVector<Pattern> m_patterns;
    QByteArray m_buffer;
};
