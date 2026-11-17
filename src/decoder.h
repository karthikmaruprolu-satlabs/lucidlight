#pragma once

#include <QByteArray>
#include <QString>

// Lucidlight extension: inline protocol decoding for the communication
// window. v1 ships a Modbus RTU decoder — frames that parse as valid
// Modbus (address, supported function, matching CRC-16) get a one-line
// human-readable annotation under the received row.
namespace ModbusDecoder {

// Returns a decoded description like
//   "MODBUS addr=1 FC=03 read-holding-regs start=100 count=2 CRC ok"
// or an empty string when the frame is not decodable Modbus RTU.
QString tryDecode(const QByteArray &frame);

// Streaming variant for live decoding: appends the received bytes to a
// per-channel accumulator (keyed by channelKey) and decodes as soon as a
// complete frame is buffered — so a Modbus frame split across several
// receives still gets its annotation. Returns the annotation for a newly
// completed frame, or an empty string. Stale partial data (> 2 s old) is
// discarded.
QString tryDecodeStream(quintptr channelKey, const QByteArray &data);

} // namespace ModbusDecoder
