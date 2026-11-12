#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// Parses Lucidlight's sequence notation into a stream of typed elements, the
// counterpart of Docklight's per-character sequence model:
//
//   Text mode:  plain characters; [0D] or [D] inserts hex byte 0x0D;
//               [[ inserts a literal '['. '?' matches exactly one arbitrary
//               byte (Docklight's F7 wildcard), '#' matches zero or one byte
//               (F8). The doubled '??' is accepted as two one-byte wildcards.
//   Hex mode:   hex digit pairs with optional separators; '?' matches one
//               byte, '#' matches zero or one, '??' is two one-byte
//               wildcards.
//   Dec mode:   decimal byte values 0..255, same wildcard tokens.
//
// Function characters (Docklight F9/F10/F11/F12) use bracket notation in
// every mode, so they can never collide with data:
//   [&NN]  inter-character delay, NN * 10 milliseconds (F9 '&')
//   [%]    break condition (F10 '%')
//   [!V]   handshake/parity control (F11 '!'): V = 001 RTS high, 002 DTR
//          high, 003 both; 016/032/048/064/080 switch parity to
//          none/odd/even/mark/space. In receive sequences V selects the
//          signal state to detect: bit0 CTS, bit1 DSR, bit2 DCD, bit3 RI.
//   [^MM VV]  bitwise comparison of the next byte (F12 '^'): matches when
//          ((byte XOR VV) AND MM) == 0.
struct ParsedBytes {
    struct Element {
        enum Kind : quint8 {
            Data,          // one literal byte
            WildOne,       // '?' — exactly one arbitrary byte
            WildZeroOrOne, // '#' — zero or one byte
            Delay,         // '&' — pause, value * 10 ms
            Break,         // '%' — break condition
            Handshake,     // '!' — RTS/DTR/parity out, signal state in
            Bitwise        // '^' — compare next byte against mask/value
        };
        Kind kind = Data;
        quint8 value = 0;      // Data: the byte. Delay: 10 ms units.
        quint8 mask = 0;       // Bitwise only: which bits to compare.
        quint8 expect = 0;     // Bitwise only: the expected bit values.
    };

    QVector<Element> elements;
    QString error;

    bool ok() const { return error.isEmpty(); }

    // The data bytes with wildcards contributing 0x00 — what a send would
    // transmit without parameters, and what older code paths consume.
    QByteArray bytes() const;
    // Compat view: true where the element is any byte wildcard.
    QVector<bool> wildcard() const;
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
