#pragma once

#include <QByteArray>
#include <QString>

// The Docklight "Checksum Specification" engine, replicated from the
// Docklight Scripting V2.4 manual (Reference > Checksum Specification).
//
// A specification names a checksum/CRC algorithm, either via a predefined
// name ("MOD256", "CRC-MODBUS", ...) or via the generic
// "CRC:width,polynomial,init,finalXOR,reflectedInput,reflectedOutput" text.
//
//   checksumSpec   MOD256 | XOR | MOD65536 | LRC | LRC-ASCII | CRC-* | CRC:...
//   result         big-endian hex bytes ("CB F4 39 26") unless requested
//                  otherwise; LRC-ASCII returns two ASCII hex characters.
class Checksum {
public:
    struct Spec {
        int width = 0;            // bits, 0 = invalid/unknown
        quint32 polynomial = 0;   // truncated polynomial
        quint32 init = 0;
        quint32 finalXor = 0;
        bool reflectIn = false;
        bool reflectOut = false;
        // Non-CRC algorithms:
        enum Kind { Crc, Mod256, Xor, Mod65536, Lrc, LrcAscii } kind = Crc;
        bool valid = false;
    };

    // Parses a checksumSpec string; !valid means the text was not recognized.
    static Spec parse(const QString &spec);

    // Computes the checksum over data; returns the result bytes the way
    // Docklight's CalcChecksum does: big-endian (most significant byte
    // first) unless the algorithm reflects its output, little-endian ASCII
    // hex for LRC-ASCII. An empty result means an invalid spec.
    static QByteArray compute(const Spec &spec, const QByteArray &data);

    // Convenience: parse + compute.
    static QByteArray compute(const QString &spec, const QByteArray &data);

    // The sequence-level wrapper used by the Edit Sequence dialogs:
    //   [ ( startPos, len ) ] checksumSpec [A] [L] [@ targetPos] [# comment]
    // Everything in brackets is optional. Returns !ok with an error text for
    // malformed input.
    struct Area {
        Spec spec;
        bool ok = false;
        QString error;
        int startPos = 0;      // 0 = default: everything before the result
        int length = 0;        // 0 = default: until the result area
        bool asciiResult = false;  // "A": result as readable ASCII hex
        bool littleEndian = false; // "L": least significant byte first
        int targetPos = 0;     // 0 = default: last bytes of the sequence
        QString comment;
    };
    static Area parseArea(const QString &areaText);

    // Applies an Area to a sequence: computes the checksum over the area and
    // writes the result bytes at the target position. Negative positions are
    // relative to the end of the sequence, as in Docklight. Returns false
    // (and sets error) when the sequence is too short for the area.
    static bool applyToSequence(const Area &area, QByteArray &sequence, QString *error);

    // Validates received data against an Area, the way a Receive Sequence
    // checksum does. Returns true when the stored checksum bytes match the
    // computed value.
    static bool validateSequence(const Area &area, const QByteArray &sequence, QString *error);

    // Renders checksum bytes as readable ASCII hex text, e.g. byte 0x7E
    // becomes the two characters "7E". Used by the "A" modifier and by
    // LRC-ASCII style results.
    static QByteArray toAsciiHex(const QByteArray &bytes, bool littleEndian);
};
