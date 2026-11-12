#pragma once

#include <QByteArray>
#include <QObject>
#include <QVector>

#include "dataformat.h"

// Watches the incoming byte stream for receive-sequence patterns and fires
// the matching trigger. This is Lucidlight's counterpart of Docklight's
// Receive Sequence Data Handler:
//
//   - '?' matches exactly one arbitrary byte, '#' matches zero or one byte.
//     At the earliest match position the longest possible match wins, so a
//     "match inside a match" is never reported (Docklight behavior).
//   - '&' elements require a minimum silent gap of value * 10 ms before the
//     next pattern element may match (pause detection).
//   - '%' elements match a break condition reported by the serial link.
//   - '!' elements match the current handshake signal state (CTS/DSR/DCD/RI
//     bits) reported by the serial link.
//   - '^' elements compare the next byte bitwise against mask/value.
//   - Optional checksum areas are validated before a match is reported; the
//     action mode decides whether matching checksums (OK), mismatching
//     checksums (WRONG) or both (BOTH) constitute a match.
class SequenceMatcher : public QObject {
    Q_OBJECT
public:
    // One consumed stream token.
    struct Token {
        enum Kind : quint8 { Data, Break, Handshake };
        quint8 kind = Data;
        quint8 value = 0;    // Data: byte. Handshake: current signal bits.
        qint64 timeMs = 0;   // Arrival time of the token.
    };

    struct Pattern {
        int sequenceId = 0;
        QVector<ParsedBytes::Element> elements;
        // Optional checksum validation over the matched window.
        bool hasChecksum = false;
        QString checksumSpec;      // full area text, parsed on use
        int checksumMode = 0;      // 0 OK, 1 WRONG, 2 BOTH
    };

    explicit SequenceMatcher(QObject *parent = nullptr);

    void setPatterns(const QVector<Pattern> &patterns);
    void reset();
    void feed(const QByteArray &data, qint64 timeMs);
    // Injects line events that data sequences cannot express.
    void feedBreak(qint64 timeMs);
    void feedHandshake(quint8 bits, qint64 timeMs);

signals:
    // Emitted once per recognized occurrence, with the tokens consumed.
    void matched(int sequenceId, const QVector<Token> &tokens);

private:
    void scan();

    QVector<Pattern> m_patterns;
    QVector<Token> m_buffer;
};
