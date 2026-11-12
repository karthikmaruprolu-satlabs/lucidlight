#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// Docklight-style data notation, shared by the send bar, the sequences, and
// the scripting DL object:
//   Text mode: plain characters, [0D] inserts a hex byte, [[ inserts a
//              literal '[', ?? matches one unknown byte when receiving.
//   Hex mode:  hex digit pairs with optional separators, ?? is a wildcard.
//   Dec mode:  decimal byte values 0..255, ?? is a wildcard.
struct ParsedBytes {
    QByteArray bytes;
    QVector<bool> wildcard;    // true where the position matched ??
    QString error;
    bool ok() const { return error.isEmpty(); }
};

class DataFormat {
public:
    enum class Format { Text, Hex, Dec };

    static ParsedBytes parse(const QString &input, Format format);
    // Display form of received bytes in the chosen view: ASCII shows
    // non-printables as a middot, hex and decimal append the ASCII gutter.
    static QString display(const QByteArray &data, Format format);
    // Canonical hex string with single spaces, e.g. "48 65 6C 6C 6F".
    static QString toHex(const QByteArray &data);
    static QByteArray fromHex(const QString &hex, QString *error = nullptr);
    // Escape a raw string so Text-mode notation round-trips it.
    static QString toTextNotation(const QByteArray &data);
};
