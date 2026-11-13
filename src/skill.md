# Lucidlight

Drive serial devices from the terminal or desktop — a serial monitor,
terminal, and scripted testing tool (a Linux take on Docklight Scripting).
Use it for automated serial/RS232 test runs, device bring-up, and protocol
experiments on Arch Linux machines.

## The one command agents need

```sh
lucidlight run <project.llproj> [--script <file.js>] [options]
```

- Loads the project (ports, sequences, display settings), opens the
  communication, runs the script, prints traffic, exits.
- **Exit code 0** = script completed, **1** = port failure, script error, or
  `DL.Fail` / failed `DL.Assert`.
- Options: `--script <file.js>`, `--duration <seconds>` (default 10; a script
  that ends earlier wins), `--port-a <dev>`, `--port-b <dev>`,
  `--mode <0|1|2>`, `--baud <rate>`, `--json`, `--out <logfile>`.
- A project file is optional: with only `--port-a` you get single-port mode
  at 115200.

### JSON output (`--json`)

One JSON object per line (NDJSON) on stdout:

```json
{"type":"tx","channel":0,"hex":"50 49 4E 47","text":"PING","t":"12:00:00.123"}
{"type":"rx","channel":0,"hex":"41 43 4B","text":"ACK","t":"12:00:00.130"}
{"type":"script","line":"device answered","error":false}
{"type":"result","ok":true,"error":"","tx":9,"rx":3,"duration":0.42}
```

`{"type":"result"}` is always the last line. Parse it for the verdict.

## Project file (`.llproj`, JSON)

```json
{
  "version": 1,
  "mode": 0,
  "channelA": { "device": "/dev/ttyUSB0", "baud": 115200,
                "dataBits": 8, "parity": 0, "stopBits": 1, "flow": 0 },
  "channelB": { "device": "", "baud": 115200, "dataBits": 8, "parity": 0, "stopBits": 1, "flow": 0 },
  "sequences": [
    { "name": "Ping", "isSend": true, "format": "text", "data": "PING[0D][0A]", "hotkey": 1 },
    { "name": "Ack", "isSend": false, "format": "text", "data": "ACK", "replyToId": -1 }
  ],
  "script": "DL.Send(\"PING\\r\"); if (!DL.WaitForSequence(\"Ack\", 1000)) DL.Fail(\"no ack\");",
  "display": { "format": "text", "timestamps": true, "delta": false }
}
```

`parity`: 0 none, 1 even, 2 odd, 3 mark, 4 space. `flow`: 0 none, 1 RTS/CTS,
2 Xon/Xoff. `format`: "text", "hex", "dec". `replyToId`: id of the send
sequence to auto-fire when the receive pattern matches.

## Data notation

- Text mode: plain characters; `[0D]` inserts one hex byte; `[[` is a
  literal `[`; `??` matches one unknown byte (receive patterns only);
  `<CR>`, `<LF>`, `<NUL>`, `<13>` style control names also parse.
- Send Sequence Parameter dialog: sequences with wildcard runs ask for
  values before sending (GUI dialog; headless auto-answer via the
  `LUCID_PARAMS` env var, values separated by `|`).
- Hex mode: `48 65 6C` pairs, `??` wildcards allowed.
- Dec mode: `72 101 108` decimal bytes, `??` wildcards allowed.

## Scripting: the DL object (JavaScript, runs off the UI thread)

```js
DL.StartCommunication();              // open the ports from the project
DL.StopCommunication();               DL.IsCommRunning();
DL.Send("AT[0D]");                    // project notation
DL.SendHex("48 45"); DL.SendStr("hi"); DL.SendBytes([1,2,3]);
DL.SendSequence("Ping");              // by name or index
DL.Receive(16, 500);                  // next 16 bytes as string, or false
DL.ReceiveLine(1000);                 // next line (up to CR/LF, terminator consumed)
DL.ReceiveStr("OK", 1000);            // true when the text appears (consumes stream)
DL.ReceiveHex("AA ?? 55", 1000);      // matched bytes as hex, or false
DL.WaitForSequence("Ack", 1000);      // true when the named receive sequence matches
DL.Wait(250); DL.Pause();
DL.Comment("note"); DL.ClearComm(); DL.ShowMessage("hello");
DL.HexToStr(h); DL.StrToHex(s); DL.HexToDec(h); DL.DecToHex(n); DL.Asc(c); DL.Chr(n);
DL.SendBreak(200); DL.SetDTR(true); DL.SetRTS(false);
DL.GetCTS(); DL.GetDSR(); DL.GetDCD(); DL.GetRI();
DL.StartTime(); DL.TimeDiff();        // ms since script start / seconds
DL.Env("DEVICE", "/dev/ttyUSB0");     // read environment variables
DL.SaveLog("/tmp/capture.log");       // dump the communication window
DL.Assert(ok, "device must answer");  // DL.Fail(message) on false
DL.Fail("reason");                    // stop and exit 1
DL.Break();                           // stop early, no failure
print("anything");                    // to the output pane / stdout
```

Blocking Receive*/WaitFor*/Wait calls never block the UI; Stop always works.

Stream semantics: every receive consumes what it matched, and anything
before or after the match stays in the buffer. When a match lands mid-line
(a sequence or ReceiveStr), the rest of that line is still there — finish
the line with `DL.ReceiveLine(timeout)` before reading the next one. When a
script fails, run exits 1 and the `result` line carries the message.

## Testing without hardware

Create a loopback with socat and point the project at the pty:

```sh
socat -d -d pty,raw,echo=0 EXEC:cat,pty,raw,echo=0 &
# the first "PTY is /dev/pts/N" line is your port
lucidlight run loopback.llproj --script test.js
```

Everything written comes straight back, so full scripts can be exercised
with no device attached.

## Desktop use

`lucidlight` opens the GUI: Start (Space) begins communication, the send bar
takes notation and Enter sends, Ctrl+F1–F12 fire sequences, the camera
button takes a Snapshot (window hidden until a trigger sequence fires),
the Script tab runs the same DL scripts interactively (Ctrl+R). Projects are `.llproj` JSON,
open/save through the portal file dialogs. The session autosaves to
`~/.local/state/lucidlight/session.json`.
