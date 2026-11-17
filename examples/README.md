# Lucidlight example projects

Load these with `File > Open…` (or `lucidlight run --script … <project>`).

## pingpong.llproj
A full ping-pong conversation in one project, run twice over a null modem
(or `socat PTY,link=/tmp/llA PTY,link=/tmp/llB,raw,echo=0`, one end per
instance). Press `Ctrl+F1` (*Ping*): the other side's *Ping received*
pattern answers with the *Pong* send sequence via reply-to, and its
arrival comments `Got a Pong at %_T` (timestamp macro) in your window.
Both sides can start the conversation.

## modbus-master.llproj / modbus-slave.llproj
A complete Modbus RTU master/slave pair over a virtual null modem:
- *master*: "Read Holding Reg" sends `01 03 00 00 00 02` plus a live
  CRC-MODBUS checksum (`(1,6)` area, little-endian), and the *Modbus Response*
  receive sequence validates the reply CRC with wildcards.
- *slave*: the *Request* receive sequence validates the request CRC and
  replies with the *Response* send sequence via `replyToId`.
Point channel A of each project at one end of the PTY pair and start
communication in both.
