#include "jshighlighter.h"

#include <QRegularExpression>

JsHighlighter::JsHighlighter(QObject *parent) : QSyntaxHighlighter(parent) {}

void JsHighlighter::setDocument(QQuickTextDocument *document) {
    if (m_document == document)
        return;
    m_document = document;
    if (m_document)
        QSyntaxHighlighter::setDocument(m_document->textDocument());
    else
        QSyntaxHighlighter::setDocument(nullptr);
}

void JsHighlighter::highlightBlock(const QString &text) {
    if (!document())
        return;

    // Multiline comments: state from the previous block continues them.
    enum { StateNone = 0, StateComment = 1 };
    const int previous = previousBlockState();
    int state = previous == StateComment ? StateComment : StateNone;
    setCurrentBlockState(StateNone);

    int at = 0;
    while (at < text.length()) {
        if (state == StateComment) {
            const int end = text.indexOf(QLatin1String("*/"), at);
            if (end < 0) {
                setFormat(at, text.length() - at, m_comment);
                setCurrentBlockState(StateComment);
                return;
            }
            setFormat(at, end + 2 - at, m_comment);
            at = end + 2;
            state = StateNone;
            continue;
        }
        // A comment runs to the end of the line.
        const int lineComment = text.indexOf(QLatin1String("//"), at);
        const int blockStart = text.indexOf(QLatin1String("/*"), at);
        if (lineComment >= 0 && (blockStart < 0 || lineComment < blockStart)) {
            setFormat(lineComment, text.length() - lineComment, m_comment);
            return;
        }
        if (blockStart >= 0) {
            const int end = text.indexOf(QLatin1String("*/"), blockStart + 2);
            if (end < 0) {
                setFormat(blockStart, text.length() - blockStart, m_comment);
                setCurrentBlockState(StateComment);
                return;
            }
            setFormat(blockStart, end + 2 - blockStart, m_comment);
            at = end + 2;
            continue;
        }
        // Strings, single or double quoted.
        const QChar quote = text.at(at);
        if (quote == QLatin1Char('"') || quote == QLatin1Char('\'')) {
            int end = at + 1;
            while (end < text.length() && text.at(end) != quote) {
                if (text.at(end) == QLatin1Char('\\'))
                    ++end;
                ++end;
            }
            const int length = qMin(end + 1, text.length()) - at;
            setFormat(at, length, m_string);
            at = end + 1;
            continue;
        }
        ++at;
    }

    static const QRegularExpression keywords(
        QStringLiteral("\\b(function|var|let|const|if|else|for|while|do|switch|case|break|continue|"
                       "return|new|try|catch|finally|throw|typeof|true|false|null|undefined)\\b"));
    auto matches = keywords.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_keyword);
    }

    static const QRegularExpression numbers(QStringLiteral("\\b(0x[0-9a-fA-F]+|\\d+(\\.\\d+)?)\\b"));
    matches = numbers.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_number);
    }

    // The DL object calls, the heart of every Lucidlight script.
    static const QRegularExpression dlCalls(QStringLiteral("\\bDL\\.[A-Za-z_]+"));
    matches = dlCalls.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        setFormat(match.capturedStart(), match.capturedLength(), m_dl);
    }
}
