#include "vbstrans.h"

#include <QRegularExpression>
#include <QStringList>
#include <QCoreApplication>
#include <cstdio>

// Line-based VBScript translator. The goal is not a full VBScript compiler
// but a faithful conversion of the constructs that appear in Docklight
// scripting: comments, Dim/Const, assignments, Sub/Function, block and
// single-line If, For...Next (with Step), For Each...In, While...Wend,
// Do...Loop variants, Select Case (including To ranges and Is), call
// statements without parentheses, & concatenation, and the common VBScript
// operator set. See tests/lucidlight_tests.cpp for manual-derived samples.

namespace VbsTrans {
namespace {

// Recognized procedure names for call-without-parens conversion; filled by
// the declaration pass. DL.* names are handled structurally.
QStringList procedureNames;

bool isKeywordLine(const QString &head)
{
    static const QStringList keywords = {
        "dim", "const", "redim", "option", "on", "set", "exit", "erase",
        "randomize", "call", "with", "rem"
    };
    return keywords.contains(head.toLower());
}

// Split on a character that is not inside a string literal.
QStringList splitOutside(const QString &text, QChar sep)
{
    QStringList parts;
    QString current;
    bool inString = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == '"') {
            // A doubled quote is an escaped quote, not a boundary.
            if (inString && i + 1 < text.size() && text.at(i + 1) == '"') {
                current += "\"\"";
                ++i;
                continue;
            }
            inString = !inString;
            current += c;
        } else if (c == '\'' && !inString) {
            break;   // VB comment
        } else if (c == sep && !inString) {
            parts.append(current);
            current.clear();
        } else {
            current += c;
        }
    }
    parts.append(current);
    return parts;
}

// Find a standalone word outside string literals; returns the index of the
// word start or -1.
int indexOfWordOutside(const QString &text, const QString &word)
{
    const int n = text.size();
    const int w = word.size();
    bool inString = false;
    for (int i = 0; i + w <= n; ++i) {
        const QChar c = text.at(i);
        if (c == '"') {
            if (inString && i + 1 < n && text.at(i + 1) == '"') {
                ++i;
                continue;
            }
            inString = !inString;
            continue;
        }
        if (inString)
            continue;
        if (text.mid(i, w).compare(word, Qt::CaseInsensitive) == 0) {
            const bool leftOk = i == 0 || !(text.at(i - 1).isLetterOrNumber()
                                            || text.at(i - 1) == '_');
            const bool rightOk = i + w >= n
                || !(text.at(i + w).isLetterOrNumber() || text.at(i + w) == '_');
            if (leftOk && rightOk)
                return i;
        }
    }
    return -1;
}

// Remove a trailing VB comment (' ...) that is not inside a string.
QString stripComment(const QString &text)
{
    bool inString = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == '"') {
            if (inString && i + 1 < text.size() && text.at(i + 1) == '"') {
                ++i;
                continue;
            }
            inString = !inString;
        } else if (c == '\'' && !inString) {
            return text.left(i);
        }
    }
    return text;
}

// VB string literals escape embedded quotes by doubling; JS uses \".
QString convertStringLiterals(const QString &expr, bool *changed)
{
    QString out;
    out.reserve(expr.size());
    bool inString = false;
    for (int i = 0; i < expr.size(); ++i) {
        const QChar c = expr.at(i);
        if (c == '"') {
            if (inString && i + 1 < expr.size() && expr.at(i + 1) == '"') {
                out += "\\\"";
                ++i;
                if (changed)
                    *changed = true;
            } else {
                inString = !inString;
                out += '"';
            }
        } else {
            out += c;
        }
    }
    return out;
}

// Replace hex/octal literals like &HFF or &O17.
QString convertRadixLiterals(const QString &expr, const QChar kind)
{
    static const QRegularExpression hexRe(
        QStringLiteral("&[Hh]([0-9A-Fa-f]+)&?"));
    static const QRegularExpression octRe(QStringLiteral("&[Oo]([0-7]+)&?"));
    const QRegularExpression &re = kind == 'h' ? hexRe : octRe;
    if (!expr.contains(re))
        return expr;
    QString result;
    int last = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(expr);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        result += expr.mid(last, m.capturedStart() - last);
        result += kind == 'h' ? QStringLiteral("0x") + m.captured(1)
                              : QStringLiteral("0o") + m.captured(1);
        last = m.capturedEnd();
    }
    result += expr.mid(last);
    return result;
}

// Convert one VB expression to a JS expression.
QString convertExpression(QString expr)
{
    expr = expr.trimmed();
    if (expr.isEmpty())
        return expr;
    bool changed = false;
    expr = convertStringLiterals(expr, &changed);
    expr = convertRadixLiterals(expr, 'h');
    expr = convertRadixLiterals(expr, 'o');

    // Word operators, constants and function aliases (outside strings).
    QString out;
    out.reserve(expr.size());
    bool inString = false;
    for (int i = 0; i < expr.size();) {
        const QChar c = expr.at(i);
        if (c == '"') {
            inString = !inString;
            out += c;
            ++i;
            continue;
        }
        if (inString) {
            out += c;
            ++i;
            continue;
        }
        if (c.isLetterOrNumber() || c == '_' || c == '$') {
            int j = i;
            while (j < expr.size()
                   && (expr.at(j).isLetterOrNumber() || expr.at(j) == '_'
                       || expr.at(j) == '$'))
                ++j;
            const QString word = expr.mid(i, j - i);
            const QString lower = word.toLower();
            // VB date/time value functions usable bare or called: Date,
            // Time, Now, Timer. An assignment target keeps its name.
            if (lower == "date" || lower == "time" || lower == "now"
                || lower == "timer") {
                int k = j;
                while (k < expr.size() && expr.at(k) == ' ')
                    ++k;
                const QChar next = k < expr.size() ? expr.at(k) : QChar();
                if (next == '(') {
                    out += word;   // call follows; keep the name
                } else if (next == '=') {
                    out += word;   // assignment target
                } else {
                    const QString call = lower == "date" ? "_Date"
                                       : lower == "time" ? "_Time"
                                       : word;
                    out += call + "()";
                }
                i = j;
                continue;
            }
            if (lower == "true")
                out += "true";
            else if (lower == "false")
                out += "false";
            else if (lower == "not")
                out += "!";
            else if (lower == "and")
                out += "&&";
            else if (lower == "or")
                out += "||";
            else if (lower == "xor")
                out += "!==";   // boolean xor; bitwise xor is rare in scripts
            else if (lower == "mod")
                out += "%";
            else if (lower == "empty" || lower == "nothing")
                out += "undefined";
            else if (lower == "msgbox")
                out += "DL.MsgBox2";
            else if (lower == "inputbox")
                out += "DL.InputBox2";
            else if (lower == "rnd")
                out += "Math.random()";
            else if (lower == "int")
                out += "VBInt";
            else if (lower == "fix")
                out += "Math.trunc";
            else if (lower == "abs")
                out += "Math.abs";
            else if (lower == "sqr")
                out += "Math.sqrt";
            else if (lower == "exp")
                out += "Math.exp";
            else if (lower == "sgn")
                out += "VBSgn";
            else if (lower == "atn")
                out += "Math.atan";
            else if (lower == "log")
                out += "Math.log";
            else if (lower == "wscript.echo")
                out += "print";
            else
                out += word;
            i = j;
            continue;
        }
        // & concatenation (but not &H literals, already replaced).
        if (c == '&') {
            out += '+';
            ++i;
            continue;
        }
        // <> inequality.
        if (c == '<' && i + 1 < expr.size() && expr.at(i + 1) == '>') {
            out += "!==";
            i += 2;
            continue;
        }
        out += c;
        ++i;
    }
    expr = out;

    // Equality: = inside an expression means comparison. Replace any =
    // that is not part of <=, >=, !=, +=, -=, *=, /=, ==, ===.
    QString eq;
    eq.reserve(expr.size());
    inString = false;
    for (int i = 0; i < expr.size(); ++i) {
        const QChar c = expr.at(i);
        if (c == '"') {
            inString = !inString;
            eq += c;
            continue;
        }
        if (!inString && c == '=') {
            const QChar prev = i > 0 ? expr.at(i - 1) : QChar();
            const QChar next = i + 1 < expr.size() ? expr.at(i + 1) : QChar();
            const bool compound = prev == '<' || prev == '>' || prev == '!'
                                  || prev == '+' || prev == '-' || prev == '*'
                                  || prev == '/';
            const bool alreadyEq = prev == '=' || next == '=';
            if (!compound && !alreadyEq)
                eq += "===";
            else
                eq += c;
            continue;
        }
        eq += c;
    }
    return eq.trimmed();
}

// Split an argument list on top-level commas.
QStringList splitArgs(const QString &args)
{
    return splitOutside(args, ',');
}

struct Block {
    enum Kind { If, For, ForEach, While, Do, Select, Sub, Function, With };
    Kind kind;
    QString extra;   // function name, or "pending"/"seen" for Select
};

class Translator {
public:
    QString run(const QStringList &lines, QStringList *warnings);

private:
    QVector<Block> m_stack;
    QStringList m_out;
    QStringList *m_warnings = nullptr;

    void push(Block::Kind kind, const QString &extra = QString());
    QString pop();
    void popSilent();
    Block::Kind topKind() const;
    QString indent() const;
    void emitLine(const QString &text) { m_out.append(indent() + text); }
    QString insideFunction() const;

    QString translateStatement(const QString &raw, const QStringList &procs);
    QString convertCall(const QString &head, const QString &rest);
    QString wrapStatement(const QString &text, const QStringList &procs);
    QString selectVar() const;
};

void Translator::push(Block::Kind kind, const QString &extra)
{
    Block b;
    b.kind = kind;
    b.extra = extra;
    m_stack.append(b);
}

QString Translator::pop()
{
    if (m_stack.isEmpty())
        return "}";
    m_stack.removeLast();
    return "}";
}

void Translator::popSilent()
{
    if (!m_stack.isEmpty())
        m_stack.removeLast();
}

Block::Kind Translator::topKind() const
{
    return m_stack.isEmpty() ? Block::If : m_stack.last().kind;
}

QString Translator::indent() const
{
    return QString(m_stack.size() * 2, ' ');
}

QString Translator::insideFunction() const
{
    for (int i = m_stack.size() - 1; i >= 0; --i)
        if (m_stack.at(i).kind == Block::Function)
            return m_stack.at(i).extra;
    return QString();
}

QString Translator::selectVar() const
{
    for (int i = m_stack.size() - 1; i >= 0; --i)
        if (m_stack.at(i).kind == Block::Select)
            return QStringLiteral("_sw%1").arg(i);
    return "_sw";
}

QString Translator::convertCall(const QString &head, const QString &rest)
{
    const QStringList args = splitArgs(rest);
    QStringList converted;
    for (const QString &arg : args) {
        const QString trimmed = arg.trimmed();
        converted.append(trimmed.isEmpty() ? QString("undefined")
                                           : convertExpression(trimmed));
    }
    return head + "(" + converted.join(", ") + ")";
}

QString Translator::wrapStatement(const QString &text, const QStringList &procs)
{
    QString stmt = translateStatement(text, procs);
    if (stmt.endsWith(';'))
        stmt.chop(1);
    return stmt;
}

QString Translator::translateStatement(const QString &raw,
                                       const QStringList &procs)
{
    QString line = raw.trimmed();
    if (line.isEmpty())
        return QString();

    // VB comment lines become JS comments.
    if (line.startsWith('\'') || line.startsWith("rem ", Qt::CaseInsensitive)
        || line.compare("rem", Qt::CaseInsensitive) == 0) {
        const QString text = line.startsWith('\'') ? line.mid(1) : line.mid(3);
        return "//" + text;
    }

    const QString lower = line.toLower();

    // Option Explicit / On Error / Randomize / Erase — no JS equivalent.
    if (lower.startsWith("option explicit"))
        return QString();
    if (lower.startsWith("on error")) {
        if (m_warnings && !m_warnings->contains("on-error"))
            m_warnings->append("on-error");
        return QString();
    }
    if (lower.startsWith("randomize"))
        return QString();
    if (lower.startsWith("erase "))
        return QString();

    // Exit statements.
    if (lower == "exit do" || lower == "exit for" || lower == "exit while")
        return "break;";
    if (lower == "exit sub" || lower == "exit function")
        return "return;";

    // Dim / Const / ReDim.
    static const QRegularExpression dimRe(
        QStringLiteral("^(dim|const|redim)\\s+(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch dimMatch = dimRe.match(line);
    if (dimMatch.hasMatch()) {
        const QString rest = dimMatch.captured(2);
        // "Dim arr(10)" allocates; plain names declare.
        QStringList parts;
        for (const QString &piece : splitArgs(rest)) {
            static const QRegularExpression arrayRe(
                QStringLiteral("^([A-Za-z_]\\w*)\\(([^)]*)\\)$"));
            const QRegularExpressionMatch am = arrayRe.match(piece.trimmed());
            if (am.hasMatch()) {
                bool ok = false;
                const int n = convertExpression(am.captured(2)).toInt(&ok);
                parts.append(QStringLiteral("%1 = new Array(%2)")
                                 .arg(am.captured(1)).arg(ok ? n + 1 : 0));
            } else {
                parts.append(piece.trimmed());
            }
        }
        return "var " + parts.join(", ") + ";";
    }

    // Sub / Function declarations.
    static const QRegularExpression subRe(
        QStringLiteral("^(sub|function)\\s+([A-Za-z_]\\w*)\\s*\\(([^)]*)\\)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch subMatch = subRe.match(line);
    if (subMatch.hasMatch()) {
        const bool isFunction = subMatch.captured(1).compare(
            "function", Qt::CaseInsensitive) == 0;
        push(isFunction ? Block::Function : Block::Sub, subMatch.captured(2));
        QString params = subMatch.captured(3);
        params.remove("ByVal", Qt::CaseInsensitive);
        params.remove("ByRef", Qt::CaseInsensitive);
        QStringList plist;
        for (const QString &p : splitArgs(params))
            if (!p.trimmed().isEmpty())
                plist.append(p.trimmed());
        return QStringLiteral("function %1(%2) {").arg(subMatch.captured(2),
                                                       plist.join(", "));
    }
    static const QRegularExpression subNoArgsRe(
        QStringLiteral("^(sub|function)\\s+([A-Za-z_]\\w*)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch subNoArgs = subNoArgsRe.match(line);
    if (subNoArgs.hasMatch()) {
        const bool isFunction = subNoArgs.captured(1).compare(
            "function", Qt::CaseInsensitive) == 0;
        push(isFunction ? Block::Function : Block::Sub, subNoArgs.captured(2));
        return QStringLiteral("function %1() {").arg(subNoArgs.captured(2));
    }
    if (lower.startsWith("end sub") || lower.startsWith("end function"))
        return pop() + "  // " + line;

    // Call statement (explicit).
    static const QRegularExpression callRe(
        QStringLiteral("^call\\s+([A-Za-z_][\\w.]*)\\s*\\((.*)\\)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch callMatch = callRe.match(line);
    if (callMatch.hasMatch())
        return callMatch.captured(1) + "(" + callMatch.captured(2) + ");";

    // Block If.
    static const QRegularExpression ifBlockRe(
        QStringLiteral("^if\\s+(.+)$"), QRegularExpression::CaseInsensitiveOption);
    if (lower.startsWith("if ")) {
        const QString rest = line.mid(3);
        const int thenIdx = indexOfWordOutside(rest, "Then");
        if (thenIdx < 0) {
            if (m_warnings)
                m_warnings->append("if-without-then: " + line);
            return "//" + line;
        }
        const QString cond = rest.left(thenIdx);
        const QString after = rest.mid(thenIdx + 4).trimmed();
        if (after.isEmpty()) {
            push(Block::If);
            return "if (" + convertExpression(cond) + ") {";
        }
        // Single-line If: optional trailing Else.
        const int elseIdx = indexOfWordOutside(after, "Else");
        if (elseIdx >= 0) {
            const QString thenPart = after.left(elseIdx).trimmed();
            const QString elsePart = after.mid(elseIdx + 4).trimmed();
            return "if (" + convertExpression(cond) + ") "
                   + wrapStatement(thenPart, procs) + "; else "
                   + wrapStatement(elsePart, procs) + ";";
        }
        return "if (" + convertExpression(cond) + ") "
               + wrapStatement(after, procs) + ";";
    }
    // ElseIf / Else.
    if (lower.startsWith("elseif ") || lower.startsWith("else if ")) {
        const QString rest = lower.startsWith("elseif ")
                                 ? line.mid(6) : line.mid(7);
        const int thenIdx = indexOfWordOutside(rest, "Then");
        const QString cond = thenIdx >= 0 ? rest.left(thenIdx) : rest;
        return "} else if (" + convertExpression(cond) + ") {";
    }
    if (lower == "else")
        return "} else {";
    if (lower.startsWith("end if"))
        return pop() + "  // End If";

    // For ... To ... [Step n]
    static const QRegularExpression forRe(
        QStringLiteral("^for\\s+([A-Za-z_]\\w*)\\s*=\\s*(.+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch forMatch = forRe.match(line);
    if (forMatch.hasMatch()) {
        static const QRegularExpression toRe(
            QStringLiteral("^(.*?)\\s+to\\s+(.+)$"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch toMatch = toRe.match(forMatch.captured(2));
        if (!toMatch.hasMatch()) {
            if (m_warnings)
                m_warnings->append("for-without-to: " + line);
            return "//" + line;
        }
        QString limit = toMatch.captured(2);
        QString step = "1";
        static const QRegularExpression stepRe(
            QStringLiteral("^(.*?)\\s+step\\s+(\\S+)$"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch stepMatch = stepRe.match(limit);
        if (stepMatch.hasMatch()) {
            limit = stepMatch.captured(1);
            step = convertExpression(stepMatch.captured(2));
        }
        push(Block::For, forMatch.captured(1));
        const bool negative = step.startsWith('-');
        return QStringLiteral("for (%1 = %2; %1 %3 %4; %1 += %5) {")
            .arg(forMatch.captured(1), convertExpression(toMatch.captured(1)),
                 negative ? ">=" : "<=", convertExpression(limit), step);
    }

    // For Each x In coll
    static const QRegularExpression forEachRe(
        QStringLiteral("^for\\s+each\\s+([A-Za-z_]\\w*)\\s+in\\s+(.+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch feMatch = forEachRe.match(line);
    if (feMatch.hasMatch()) {
        push(Block::ForEach, feMatch.captured(1));
        static int iter = 0;
        const QString it = QStringLiteral("_e%1").arg(iter++);
        return QStringLiteral("for (var %1 of (%2)) { var %3 = %1;")
            .arg(it, convertExpression(feMatch.captured(2)), feMatch.captured(1));
    }

    if (lower == "next" || lower.startsWith("next ")) {
        if (topKind() == Block::For || topKind() == Block::ForEach)
            return pop();
        if (m_warnings)
            m_warnings->append("next-without-for: " + line);
        return "//" + line;
    }

    // While ... Wend
    if (lower.startsWith("while ") || lower == "while") {
        push(Block::While);
        return "while (" + convertExpression(line.mid(5)) + ") {";
    }
    if (lower == "wend") {
        if (topKind() == Block::While)
            return pop();
        if (m_warnings)
            m_warnings->append("wend-without-while: " + line);
        return "//" + line;
    }

    // Do ... Loop
    if (lower.startsWith("do ") || lower == "do") {
        const QString rest = line == "do" || line.size() < 2
                                 ? QString() : line.mid(2).trimmed();
        if (rest.startsWith("while", Qt::CaseInsensitive)) {
            push(Block::Do, "pre");
            return "while (" + convertExpression(rest.mid(5)) + ") {";
        }
        if (rest.startsWith("until", Qt::CaseInsensitive)) {
            push(Block::Do, "pre");
            return "while (!(" + convertExpression(rest.mid(5)) + ")) {";
        }
        push(Block::Do, "post");
        return "do {";
    }
    if (lower.startsWith("loop")) {
        const QString rest = line.mid(4).trimmed();
        if (topKind() != Block::Do) {
            if (m_warnings)
                m_warnings->append("loop-mismatch: " + line);
            return "//" + line;
        }
        if (rest.startsWith("while", Qt::CaseInsensitive)) {
            if (m_stack.last().extra == "pre")
                return pop() + "  // Loop While";
            if (m_stack.last().extra == "pre")
                return pop() + "  // Loop While";
            popSilent();
            return "} while (" + convertExpression(rest.mid(5)) + ");"
                   + "  // Loop While";
        }
        if (rest.startsWith("until", Qt::CaseInsensitive)) {
            if (m_stack.last().extra == "pre")
                return pop() + "  // Loop Until";
            if (m_stack.last().extra == "pre")
                return pop() + "  // Loop Until";
            popSilent();
            return "} while (!(" + convertExpression(rest.mid(5)) + "));"
                   + "  // Loop Until";
        }
        // Bare Loop closes both pre- and post-condition Do blocks.
        return pop() + "  // Loop";
    }

    // Select Case → if/else chain, evaluating the expression once.
    if (lower.startsWith("select case")) {
        push(Block::Select, "pending");
        return QStringLiteral("var %1 = (%2); {")
            .arg(selectVar(), convertExpression(line.mid(11)));
    }
    static const QRegularExpression caseRe(
        QStringLiteral("^case\\s+(.+)$"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch caseMatch = caseRe.match(line);
    const bool caseElse = lower.startsWith("case else")
                          || lower == "case";
    if (!caseElse && caseMatch.hasMatch() && topKind() == Block::Select) {
        const QString sw = selectVar();
        QStringList conditions;
        for (const QString &item : splitArgs(caseMatch.captured(1))) {
            const QString t = item.trimmed();
            static const QRegularExpression toRange(
                QStringLiteral("^(.*)\\s+to\\s+(.*)$"),
                QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch range = toRange.match(t);
            if (range.hasMatch()) {
                conditions.append(QStringLiteral("(%1 >= %2 && %1 <= %3)")
                                      .arg(sw, convertExpression(range.captured(1)),
                                           convertExpression(range.captured(2))));
            } else if (t.startsWith("Is", Qt::CaseInsensitive)) {
                conditions.append(QStringLiteral("(%1 %2)")
                                      .arg(sw, convertExpression(t.mid(2))));
            } else {
                conditions.append(QStringLiteral("(%1 === %2)")
                                      .arg(sw, convertExpression(t)));
            }
        }
        const bool first = m_stack.last().extra == "pending";
        m_stack.last().extra = "seen";
        return (first ? "if (" : "} else if (") + conditions.join(" || ") + ") {";
    }
    if (lower.startsWith("case else") && topKind() == Block::Select) {
        const bool first = m_stack.last().extra == "pending";
        m_stack.last().extra = "seen";
        return first ? "else {" : "} else {";
    }
    if (lower.startsWith("end select")) {
        const bool first = m_stack.last().extra == "pending";
        return pop() + (first ? " }" : " }") + "  // End Select";
    }

    // With obj ... End With.
    if (lower.startsWith("with ")) {
        push(Block::With);
        return QStringLiteral("var _with%1 = (%2); {")
            .arg(QString::number(m_stack.size() - 1),
                 convertExpression(line.mid(5)));
    }
    if (lower.startsWith("end with")) {
        if (topKind() == Block::With)
            return pop() + "  // End With";
        return "//" + line;
    }

    // Assignment or call.
    static const QRegularExpression setRe(
        QStringLiteral("^set\\s+(.+)$"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch setMatch = setRe.match(line);
    if (setMatch.hasMatch())
        line = setMatch.captured(1);

    static const QRegularExpression assignRe(
        QStringLiteral("^([A-Za-z_][\\w.]*)(\\([^)]*\\))?((?:\\.[A-Za-z_][\\w.]*)*)\\s*=(?!=)\\s*(.+)$"));
    const QRegularExpressionMatch assign = assignRe.match(line);
    if (assign.hasMatch()) {
        const QString fn = insideFunction();
        if (!fn.isEmpty()
            && assign.captured(1).compare(fn, Qt::CaseInsensitive) == 0)
            return "return " + convertExpression(assign.captured(4)) + ";";
        return assign.captured(1) + (assign.captured(2).isNull()
                                         ? QString() : assign.captured(2))
            + (assign.captured(3).isNull() ? QString() : assign.captured(3))
            + " = " + convertExpression(assign.captured(4)) + ";";
    }

    // Call without parentheses: head followed by argument text. Only when
    // the head is a known procedure, a DL method, or a VBScript builtin.
    static const QRegularExpression bareCallRe(
        QStringLiteral("^([A-Za-z_][\\w.]*)(\\s+(.*))?$"));
    const QRegularExpressionMatch bare = bareCallRe.match(line);
    if (bare.hasMatch()) {
        const QString head = bare.captured(1);
        const QString rest = bare.captured(3).isNull()
                                 ? QString() : bare.captured(3);
        const bool isProc = procs.contains(head, Qt::CaseInsensitive);
        const bool isDl = head.startsWith("DL.", Qt::CaseInsensitive);
        static const QStringList builtins = {
            "MsgBox", "InputBox", "Print", "Echo", "Beep"
        };
        const bool isBuiltin =
            builtins.contains(head, Qt::CaseInsensitive)
            || head.compare("WScript.Echo", Qt::CaseInsensitive) == 0;
        if ((isProc || isDl || isBuiltin) && !rest.startsWith('=')) {
            QString call = convertCall(head, rest);
            if (head.compare("MsgBox", Qt::CaseInsensitive) == 0)
                call.replace(0, 6, "DL.MsgBox2");
            if (head.compare("InputBox", Qt::CaseInsensitive) == 0)
                call.replace(0, 8, "DL.InputBox2");
            if (head.compare("WScript.Echo", Qt::CaseInsensitive) == 0
                || head.compare("Echo", Qt::CaseInsensitive) == 0
                || head.compare("Print", Qt::CaseInsensitive) == 0)
                call = "print" + call.mid(call.indexOf('('));
            return call + ";";
        }
        // Bare expression statement (e.g. a lone variable or call with ()).
        return convertExpression(line) + ";";
    }
    return convertExpression(line) + ";";
}

QString Translator::run(const QStringList &lines, QStringList *warnings)
{
    m_warnings = warnings;
    m_stack.clear();
    m_out.clear();

    // Pass 1: collect procedure names for call-without-parens detection.
    QStringList procs;
    static const QRegularExpression procRe(
        QStringLiteral("^\\s*(sub|function)\\s+([A-Za-z_]\\w*)"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString &line : lines) {
        const QRegularExpressionMatch m = procRe.match(line);
        if (m.hasMatch())
            procs.append(m.captured(2));
    }

    for (const QString &line : lines) {
        // VB statement separator ":" — but not inside single-line If/loop
        // headers where the trailing statements belong to the block.
        const QString head = stripComment(line).trimmed();
        static const QRegularExpression controlRe(
            QStringLiteral("^(if|for|do|while|select|case|else|elseif|wend|loop|"
                           "end|sub|function|next)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        QStringList statements;
        if (controlRe.match(head).hasMatch()) {
            statements.append(line);
        } else {
            statements = splitOutside(line, ':');
            if (statements.size() > 1) {
                // Only the first statement may carry a leading comment.
                statements[0] = line.trimmed();
            }
        }
        for (const QString &statement : statements) {
            const QString commentOnly = statement.trimmed();
            const QString trimmed = stripComment(statement).trimmed();
            if (trimmed.isEmpty()) {
                if (commentOnly.startsWith('\''))
                    m_out.append(indent() + "//" + commentOnly.mid(1));
                else if (commentOnly.compare("rem", Qt::CaseInsensitive) == 0
                         || commentOnly.startsWith("rem ",
                                                   Qt::CaseInsensitive))
                    m_out.append(indent() + "//" + commentOnly.mid(3));
                continue;
            }
            m_out.append(indent() + translateStatement(trimmed, procs));
        }
    }
    while (!m_stack.isEmpty()) {
        if (qEnvironmentVariableIsSet("LUCID_DUMP_VBS"))
            fprintf(stderr, "[vbs] unclosed block kind=%d extra=%s\n",
                    int(m_stack.last().kind), qPrintable(m_stack.last().extra));
        m_out.append(pop());
    }
    return m_out.join('\n');
}

}  // namespace

bool looksLikeVbScript(const QString &source)
{
    static const QRegularExpression markers[] = {
        QRegularExpression(QStringLiteral("\\bDim\\b")),
        QRegularExpression(QStringLiteral("\\bEnd\\s+(If|Sub|Function|Select)\\b")),
        QRegularExpression(QStringLiteral("\\bWend\\b")),
        QRegularExpression(QStringLiteral("\\bSelect\\s+Case\\b")),
        QRegularExpression(QStringLiteral("\\bExit\\s+(Sub|Function|Do|For)\\b")),
        QRegularExpression(QStringLiteral("\\bOption\\s+Explicit\\b")),
        QRegularExpression(QStringLiteral("\\bOn\\s+Error\\b")),
        QRegularExpression(QStringLiteral("\\bMsgBox\\b")),
        QRegularExpression(QStringLiteral("\\bInputBox\\b")),
        QRegularExpression(QStringLiteral("\\bWhile\\b[^\\n]*\\bWend\\b")),
        QRegularExpression(QStringLiteral("^[ \\t]*DL\\.\\w+[ \\t]+[\"A-Za-z0-9]"),
                           QRegularExpression::MultilineOption),
        QRegularExpression(QStringLiteral("\"[^\"]*\"[ \\t]*&")),
        QRegularExpression(QStringLiteral("^[ \\t]*For\\s+\\w+\\s*="),
                           QRegularExpression::MultilineOption),
    };
    for (const QRegularExpression &re : markers)
        if (source.contains(re))
            return true;
    return false;
}

QString toJs(const QString &source, QStringList *warnings)
{
    // Join line continuations (" _").
    QStringList rawLines;
    QString pending;
    const QStringList parts = source.split('\n');
    for (const QString &part : parts) {
        QString line = part;
        while (line.endsWith('\r'))
            line.chop(1);
        const QString trimmed = line.trimmed();
        if (trimmed.endsWith(" _") || trimmed == "_") {
            pending += line.left(line.lastIndexOf('_'));
            continue;
        }
        rawLines.append(pending + line);
        pending.clear();
    }
    if (!pending.isEmpty())
        rawLines.append(pending);

    Translator translator;
    QString js = translator.run(rawLines, warnings);
    return "/*vbs*/\n" + js + "\n";
}

}  // namespace VbsTrans
