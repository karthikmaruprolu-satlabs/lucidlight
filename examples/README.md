# Lucidlight example projects

Load these with `File > Open…` (or `lucidlight run --script … <project>`).

## pingpong.llproj
The manual's "Ping Pong" getting-started flow: send the *Ping* sequence
(hotkey Ctrl+F1) and the *Pong received* trigger comments with a `%_T`
timestamp macro when the answer arrives. Run two instances connected by a
null modem (or `socat PTY,link=/tmp/llptyA PTY,link=/tmp/llptyB,raw,echo=0`)
and send Ping from one, watch the comment appear in the other.

## modbus-master.llproj / modbus-slave.llproj
A complete Modbus RTU master/slave pair over a virtual null modem:
- *master*: "Read Holding Reg" sends `01 03 00 00 00 02` plus a live
  CRC-MODBUS checksum (`(1,6)` area, little-endian), and the *Modbus Response*
  receive sequence validates the reply CRC with wildcards.
- *slave*: the *Request* receive sequence validates the request CRC and
  replies with the *Response* send sequence via `replyToId`.
Point channel A of each project at one end of the PTY pair and start
communication in both.
