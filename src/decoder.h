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

} // namespace ModbusDecoder
