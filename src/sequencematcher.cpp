#include "sequencematcher.h"

SequenceMatcher::SequenceMatcher(QObject *parent) : QObject(parent) {}

void SequenceMatcher::setPatterns(const QVector<Pattern> &patterns) {
    m_patterns = patterns;
    reset();
}

void SequenceMatcher::reset() {
    m_buffer.clear();
}

namespace {
// First occurrence of a pattern in the buffer, honoring ?? wildcards.
int indexOfPattern(const SequenceMatcher::Pattern &pattern, const QByteArray &buffer) {
    const int size = pattern.bytes.size();
    if (size == 0 || buffer.size() < size)
        return -1;
    const char *data = buffer.constData();
    const bool hasMask = pattern.wildcard.size() == size;
    for (int start = 0; start + size <= buffer.size(); ++start) {
        bool ok = true;
        for (int i = 0; i < size; ++i) {
            // A missing mask means every byte is literal.
            if ((!hasMask || !pattern.wildcard.at(i)) && data[start + i] != pattern.bytes.at(i)) {
                ok = false;
                break;
            }
        }
        if (ok)
            return start;
    }
    return -1;
}
}  // namespace

// Find the earliest match across all patterns, consume the stream up to its
// end, emit the trigger, and repeat, so several matches inside one burst all
// fire in order.
void SequenceMatcher::scan() {
    bool again = true;
    while (again && !m_buffer.isEmpty()) {
        again = false;
        int bestStart = -1, bestEnd = -1, bestId = 0, bestLength = 0;
        for (const Pattern &pattern : m_patterns) {
            const int found = indexOfPattern(pattern, m_buffer);
            if (found < 0)
                continue;
            // Earliest match wins; at the same position the longer pattern wins.
            if (bestStart < 0 || found < bestStart
                || (found == bestStart && pattern.bytes.size() > bestLength)) {
                bestStart = found;
                bestEnd = found + pattern.bytes.size();
                bestId = pattern.sequenceId;
                bestLength = pattern.bytes.size();
            }
        }
        if (bestStart < 0)
            break;
        const QByteArray matchedBytes = m_buffer.left(bestEnd);
        m_buffer.remove(0, bestEnd);
        emit matched(bestId, matchedBytes);
        again = true;
    }
}

void SequenceMatcher::feed(const QByteArray &data) {
    if (m_patterns.isEmpty())
        return;
    m_buffer.append(data);
    // Bound the carry-over buffer: patterns are short, so anything older than
    // 4 KiB can never match a future pattern.
    if (m_buffer.size() > 4096)
        m_buffer.remove(0, m_buffer.size() - 4096);
    scan();
}
