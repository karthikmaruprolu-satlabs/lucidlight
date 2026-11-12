#include "sequencematcher.h"

#include "checksum.h"

namespace {

// Longest match of a pattern against tokens starting at `start`.
// Returns the end index (exclusive), or -1. '#' elements prefer consuming a
// byte (longest match), with backtracking; the earliest start with any match
// wins, and at that position the longest match wins, matching Docklight's
// "no match inside a match" rule.
int matchLength(const QVector<ParsedBytes::Element> &elements, int elementIndex,
                const QVector<SequenceMatcher::Token> &tokens, int start,
                const QVector<qint64> &lastDataTime) {
    if (elementIndex == elements.size())
        return start;
    const ParsedBytes::Element &e = elements.at(elementIndex);
    const int count = tokens.size();
    switch (e.kind) {
    case ParsedBytes::Element::Data:
        if (start < count && tokens.at(start).kind == SequenceMatcher::Token::Data
            && tokens.at(start).value == e.value)
            return matchLength(elements, elementIndex + 1, tokens, start + 1, lastDataTime);
        return -1;
    case ParsedBytes::Element::WildOne:
        if (start < count && tokens.at(start).kind == SequenceMatcher::Token::Data)
            return matchLength(elements, elementIndex + 1, tokens, start + 1, lastDataTime);
        return -1;
    case ParsedBytes::Element::WildZeroOrOne: {
        // Greedy: try consuming one byte first, then zero.
        if (start < count && tokens.at(start).kind == SequenceMatcher::Token::Data) {
            const int taken = matchLength(elements, elementIndex + 1, tokens, start + 1, lastDataTime);
            if (taken >= 0)
                return taken;
        }
        return matchLength(elements, elementIndex + 1, tokens, start, lastDataTime);
    }
    case ParsedBytes::Element::Delay: {
        // The gap before the next data token must be at least value * 10 ms.
        // Only meaningful between two data tokens; a delay at the very start
        // compares against the time the previous token was consumed.
        int scan = start;
        // Skip non-data tokens such as breaks; they do not affect silence.
        while (scan < count && tokens.at(scan).kind != SequenceMatcher::Token::Data)
            ++scan;
        if (scan >= count)
            return -1;   // need the following character to measure the pause
        const qint64 previous = lastDataTime.value(qMax(0, start - 1), 0);
        if (tokens.at(scan).timeMs - previous < qint64(e.value) * 10)
            return -1;
        return matchLength(elements, elementIndex + 1, tokens, scan, lastDataTime);
    }
    case ParsedBytes::Element::Break:
        if (start < count && tokens.at(start).kind == SequenceMatcher::Token::Break)
            return matchLength(elements, elementIndex + 1, tokens, start + 1, lastDataTime);
        return -1;
    case ParsedBytes::Element::Handshake:
        if (start < count && tokens.at(start).kind == SequenceMatcher::Token::Handshake)
            return matchLength(elements, elementIndex + 1, tokens, start + 1, lastDataTime);
        return -1;
    case ParsedBytes::Element::Bitwise:
        if (start < count && tokens.at(start).kind == SequenceMatcher::Token::Data
            && ((tokens.at(start).value ^ e.expect) & e.mask) == 0)
            return matchLength(elements, elementIndex + 1, tokens, start + 1, lastDataTime);
        return -1;
    }
    return -1;
}

// The data bytes a match consumed, in order.
QByteArray matchBytes(const QVector<SequenceMatcher::Token> &tokens, int start, int end) {
    QByteArray out;
    for (int i = start; i < end; ++i)
        if (tokens.at(i).kind == SequenceMatcher::Token::Data)
            out.append(char(tokens.at(i).value));
    return out;
}

}  // namespace

SequenceMatcher::SequenceMatcher(QObject *parent) : QObject(parent) {}

void SequenceMatcher::setPatterns(const QVector<Pattern> &patterns) {
    m_patterns = patterns;
    for (Pattern &pattern : m_patterns) {
        if (!pattern.hasChecksum)
            continue;
        // The checksum result bytes arrive with the sender's value, so those
        // positions must stay wildcards during matching. Byte positions count
        // matching slots (data/wildcards consume one element each).
        const Checksum::Area area = Checksum::parseArea(pattern.checksumSpec);
        QVector<int> byteToElement;
        for (int i = 0; i < pattern.elements.size(); ++i) {
            switch (pattern.elements.at(i).kind) {
            case ParsedBytes::Element::Data:
            case ParsedBytes::Element::WildOne:
            case ParsedBytes::Element::WildZeroOrOne:
            case ParsedBytes::Element::Bitwise:
                byteToElement.append(i);
                break;
            default:
                break;
            }
        }
        const QVector<int> positions =
            Checksum::resultPositions(area, byteToElement.size());
        for (int pos : positions) {
            if (pos >= 0 && pos < byteToElement.size())
                pattern.elements[byteToElement.at(pos)].kind =
                    ParsedBytes::Element::WildOne;
        }
    }
    reset();
}

void SequenceMatcher::reset() {
    m_buffer.clear();
}

void SequenceMatcher::feed(const QByteArray &data, qint64 timeMs) {
    for (int i = 0; i < data.size(); ++i) {
        Token token;
        token.kind = Token::Data;
        token.value = uchar(data.at(i));
        token.timeMs = timeMs;
        m_buffer.append(token);
    }
    if (m_buffer.size() > 65536)
        m_buffer.remove(0, m_buffer.size() - 65536);
    scan();
}

void SequenceMatcher::feedBreak(qint64 timeMs) {
    Token token;
    token.kind = Token::Break;
    token.timeMs = timeMs;
    m_buffer.append(token);
    scan();
}

void SequenceMatcher::feedHandshake(quint8 bits, qint64 timeMs) {
    Token token;
    token.kind = Token::Handshake;
    token.value = bits;
    token.timeMs = timeMs;
    m_buffer.append(token);
    scan();
}

// Find the earliest match across all patterns, consume the stream up to its
// end, emit the trigger, and repeat, so several matches inside one burst all
// fire in order.
void SequenceMatcher::scan() {
    // Arrival times of the data tokens, used by delay elements.
    QVector<qint64> lastDataTime;
    lastDataTime.reserve(m_buffer.size());
    for (const Token &t : m_buffer)
        lastDataTime.append(t.timeMs);

    bool again = true;
    while (again && !m_buffer.isEmpty()) {
        again = false;
        int bestStart = -1, bestEnd = -1, bestId = 0, bestLength = 0;
        QVector<Token> bestTokens;
        for (const Pattern &pattern : m_patterns) {
            for (int start = 0; start < m_buffer.size(); ++start) {
                const int end = matchLength(pattern.elements, 0, m_buffer, start, lastDataTime);
                if (end < 0)
                    continue;
                if (pattern.hasChecksum) {
                    // The checksum decides whether this match counts.
                    const Checksum::Area area = Checksum::parseArea(pattern.checksumSpec);
                    const QByteArray received = matchBytes(m_buffer, start, end);
                    QString error;
                    const bool ok = Checksum::validateSequence(area, received, &error);
                    if (pattern.checksumMode == 0 && !ok)
                        continue;   // Detect Checksum OK: mismatch = no match
                    if (pattern.checksumMode == 1 && ok)
                        continue;   // Checksum Wrong: matching = no match
                    // BOTH always matches.
                }
                if (bestStart < 0 || start < bestStart
                    || (start == bestStart && end - start > bestLength)) {
                    bestStart = start;
                    bestEnd = end;
                    bestId = pattern.sequenceId;
                    bestLength = end - start;
                    bestTokens = m_buffer.mid(start, end - start);
                }
                break;   // earliest start for this pattern found
            }
        }
        if (bestStart < 0)
            break;
        m_buffer.remove(0, bestEnd);
        emit matched(bestId, bestTokens);
        again = true;
    }
}
