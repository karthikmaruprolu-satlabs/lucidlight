# Lucidlight

<p align="center"><img src="docs/screenshot.png" alt="Lucidlight" width="720"></p>

A serial monitor, terminal, and test scripting tool for Linux — built in the
spirit of [Docklight Scripting](https://www.docklight.de/): watch the bytes
flow, poke the device, automate the whole conversation with a script.

Current release: **0.3.3** · 105 tests · CI-green on every push.

## Features

- **Serial terminal** — open any `/dev/ttyUSB*`, `/dev/ttyACM*`, `/dev/ttyS*`,
  `/dev/ttyAMA*` (or a by-id path) at 300…4000000 baud, 5–8 data bits,
  none/even/odd/mark/space parity, 1–2 stop bits, no/RTS-CTS/Xon-Xoff flow,
  plus BREAK signaling. Implemented directly on POSIX termios, so it only
  needs Qt. **Network channels** work the same way: `TCP:host:port` (client),
  `SERVER:port` (accepts one connection), `TCPCLIENT:host:port`,
  `UDP:host:port`, `UDP:port`, plus local Unix domain sockets `UNIX:/path`
  (client), `UNIXCLIENT:/path`, and `UNIXSERVER:/path` (listener) — type them
  into the device field.
- **Three communication modes**, like Docklight:
  - *Single* — one port, send and receive.
  - *Redirect* — transmit on port A, receive the reply on port B.
  - *Monitor* — two read-only taps between two devices, color-coded by side,
    with Docklight's **Data Forwarding** (RX on one tap re-transmitted on the
    other, toggleable).
- **Communication window** — every burst timestamped, tagged RX/TX (+A/B in
  monitor mode), shown as ASCII, HEX, DEC, or **Binary** (Ctrl+Tab cycles).
  Contents filter (all / channel 1 / channel 2 / comments), optional
  control-character names (`<CR><LF>`), pause-detection stamps. Freeze the
  view while data keeps flowing, search with glowing matches
  (`Ctrl+F`, representation-aware), copy any line as text or hex, and the
  **Snapshot** camera button: hide the window while it waits for a trigger
  sequence, then reveal the buffered data with the trigger mid-window and
  stop.
- **Microsecond timing** — a toolbar toggle switches every stamp to
  seconds-only with 6 decimals (CLOCK_REALTIME, transport-independent:
  serial, TCP, UDP, UNIX all get the same precision), plus per-row delta
  columns. Log files follow the window's mode.
- **Send / receive sequences** — named snippets in Text/Hex/Dec/Binary
  notation with wildcard parameters, function characters (`&` delay, `%`
  break, `!` handshake), **checksum areas** (MOD256, XOR, LRC, LRC-ASCII,
  CRC-7/8/16/32, DOW, MODBUS, CCITT, XMODEM… or a generic
  `CRC:width,poly,init,xor,reflIn,reflOut` spec, with the manual's
  `(start,len)` area, `L` little-endian and `@target` modifiers), bound to
  **Ctrl+F1–F12** so they fire anywhere, optional auto-repeat, per-sequence
  **parameters** (send-time fill-ins with auto-answered dialogs in headless
  runs), reply-to chains, and **receive triggers**: a receive pattern (with
  `?`/`#` wildcards and checksum validation) that answers, comments with
  `%_` macros, snapshots, or stops the run.
- **Sequence library** — File → Export/Import shares all sequences between
  projects as a portable `.llseq` file (ids remapped, name collisions
  deduplicated on import).
- **Scripting** — JavaScript with a `DL` object that mirrors Docklight's
  VBScript API: `SendSequence`, `WaitForSequence`, `DirectSend`,
  `CalcChecksum`, `ConvertSequenceData`, channel settings, handshake
  signals, `OnSend_`/`OnReceive_` event procedures, `MsgBox2`/`InputBox2`,
  file logging (`_asc/_hex/_dec/_bin` as txt/htm/rtf/md/csv), FileInput/
  FileOutput, side channels 3–10, and more. **Classic Docklight VBScript
  samples are auto-translated** — `With` blocks, single-line `If/Else`,
  `Select Case`, line continuations and all. `DL.Dispatch("helper.js")` runs
  additional scripts **in parallel**, each in its own worker thread sharing
  the channels (up to 8). Blocking `DL.Receive*` calls park the script, never
  the UI; a runaway script is always stoppable. Editor with highlighting,
  line numbers, external-editor integration (`%FILE%`/`%LINE%`, live reload),
  and an output pane. Scripts can auto-run when a project opens.
- **Protocol decoder** — toggle *Decode Modbus RTU in the log* (or
  `DL.SetModbusDecode(true)`) and every received frame with a valid
  CRC-16/MODBUS is annotated inline: `MODBUS addr=1 FC=3 read-holding-regs
  2 regs: 1000 1010 CRC ok`. Requests, responses, writes and exception
  frames; anything else passes untouched.
- **Robustness features** — *auto-reopen* retries a failed/dropped serial
  channel every 2 s while communication runs (USB unplug/replug recovery),
  *high-priority timing* raises scheduler priority best-effort
  (rtkit → `sched_setscheduler` → nice) for tighter stamp accuracy.
- **Log exports** — save the communication window (or live-log it) as
  `.log/.txt` (Docklight format), `.htm` (full HTML with customizable CSS via
  `DL.SetHtmlStyle`), `.rtf`, `.md` (Markdown table), or `.csv` (strict
  RFC 4180: time, direction, channel, data_hex, data_ascii — spreadsheet and
  pandas friendly). PDF print via Ctrl+P. Timestamps follow the µs mode.
- **Agent-friendly** — the same engine runs headless with NDJSON output,
  exit codes, and test verdicts (`DL.Assert`/`DL.Fail`), `--version`, a man
  page, and a bundled agent skill: `lucidlight skill install`.
- **Projects** — one `.llproj` JSON file keeps mode, both port configs, all
  sequences, the script, and display options, with the manual's
  *run script on project open* option. Recent-files menu (persisted),
  window geometry remembered. The session autosaves to
  `~/.local/state/lucidlight/`.
- **Headless runs** — drive the same engine from the shell for CI and agents:
  `lucidlight run project.llproj --script test.js`.

## Install

```sh
cd lucidlight
./install.sh        # builds and installs via makepkg, shows up in the launcher
```

Or with the prebuilt package:

```sh
sudo pacman -U pkgbuild/lucidlight-0.3.3-1-x86_64.pkg.tar.zst
```

Or manually: `./build.sh` and copy `build/lucidlight` somewhere on `$PATH`.
Serial devices need your user in the `uucp` group (Omarchy sets this up;
Debian/Ubuntu use `dialout`). Everything except the Arch package and the
desktop theming is portable Linux — POSIX termios, PTYs, and Qt 6.

## Data notation

Used everywhere: the send bar, sequences, and `DL.Send`.

| Input | Meaning |
|---|---|
| `Hello` | literal characters (UTF-8) |
| `[0D]` | one hex byte (`1` or `2` digits) |
| `[[` | a literal `[` |
| `48 65 6C` | in Hex mode: byte pairs, `??` = one unknown byte |
| `72 101 108` | in Dec mode: decimal bytes, `??` = one unknown byte |
| `??` | receive patterns only: matches one arbitrary byte |

Checksum note: send-sequence CRCs default to MSB-first byte order (like
Docklight's CalcChecksum); append `L` for little-endian — real Modbus RTU
needs `CRC-MODBUS L`.

## Shortcuts

| Key | Action |
|---|---|
| `Space` | Start / stop communication |
| `Enter` | Send the typed data |
| `F1`–`F12` | Send the sequence bound to that key |
| `Ctrl+F1`–`F12` | Send the sequence bound to that key, anywhere |
| `Shift+F6` | Stop the script (and all dispatched scripts) |
| `Ctrl+R` | Run the script |
| `Ctrl+K` | Clear the window |
| `Ctrl+F` | Search in the data |
| `Ctrl+B` | Toggle the sequence panel |
| `Ctrl+T` | Keyboard console on/off |
| `Ctrl+O` / `Ctrl+S` | Open / save the project |
| `Ctrl+Shift+S` | Save the communication log |
| `Ctrl+P` | Print the communication window (PDF) |
| `?` | Help overlay |
| `Ctrl+Q` | Quit |

## Scripting — the `DL` object

Scripts are JavaScript, executed in a worker thread so the window stays alive.
`print(...)` writes to the output pane. Blocking calls check for Stop
continuously, and `DL.Break()` ends the script early.

```js
// Ask for the firmware version and check the answer
DL.StartCommunication();
DL.Send("ATI\r");
if (DL.ReceiveStr("OK", 1000)) {
    var reply = DL.Receive(16, 500);          // exactly 16 bytes, or false
    DL.Comment("Reply: " + DL.HexToStr(reply));
} else {
    DL.ShowMessage("No answer from the device");
}
DL.StopCommunication();
```

### Communication
| Call | Meaning |
|---|---|
| `DL.StartCommunication()` / `DL.StopCommunication()` | open the ports configured in the UI / close them |
| `DL.IsCommRunning()` | is the link up? |
| `DL.SetCommMode(mode)` | 0 single, 1 redirect, 2 monitor (while stopped) |
| `DL.ClearComm()` | wipe the communication window |
| `DL.Dispatch("file.js")` | run another script in parallel (own thread, shared comm; max 8) |
| `DL.SetModbusDecode(on)` / `DL.GetModbusDecode()` | toggle the Modbus RTU log decoder |

### Sending
| Call | Meaning |
|---|---|
| `DL.Send(data)` | project notation (Text mode: `[0D]` etc.) |
| `DL.SendHex("48 65")` / `DL.SendStr(text)` | raw hex / UTF-8 text |
| `DL.SendBytes([1, 2, 3])` | JS array of 0–255 |
| `DL.SendSequence("Name", customData)` | send a sequence by name (or index), optional data override |
| `DL.SendBreak(ms)` / `DL.SetDTR(b)` / `DL.SetRTS(b)` | line control |
| `DL.GetCTS()` / `DL.GetDSR()` / `DL.GetDCD()` / `DL.GetRI()` | read modem lines |
| `DL.GetKeyState(vk)` | Shift(16)/Ctrl(17)/Alt(18) pressed? (-1/0) |
| `DL.DirectSend(channel, data, fmt)` | send on side channels 3–10 without logging |

### Receiving (blocks the script, never the UI)
| Call | Meaning |
|---|---|
| `DL.Receive(count, timeoutMs)` | next `count` bytes as a string, or `false` on timeout |
| `DL.ReceiveLine(timeoutMs)` | next line without its CR/LF, or `false` |
| `DL.ReceiveStr(text, timeoutMs)` | `true` when `text` appears; the stream is consumed up to it |
| `DL.ReceiveHex("AA ?? 55", timeoutMs)` | hex of the matched bytes (wildcards allowed), or `false` |

### Timing, output, helpers
| Call | Meaning |
|---|---|
| `DL.Wait(ms)` / `DL.Pause()` | delay / process pending events |
| `DL.StartTime()` / `DL.TimeDiff()` | milliseconds since start / seconds as a float |
| `DL.Comment(text)` | write a note into the communication window |
| `DL.ShowMessage(text)` | toast for the user |
| `DL.Break()` | stop the script |
| `DL.Fail(message)` | stop and fail the run (headless exit code 1) |
| `DL.Assert(condition, message)` | `Fail` when the condition is false |
| `DL.WaitForSequence(name, timeoutMs)` | wait for a named receive sequence to match |
| `DL.SetMicroStamps(on)` / `DL.GetMicroStamps()` | µs-precision seconds-only stamps |
| `DL.SetHtmlStyle(css)` / `DL.GetHtmlStyle()` | CSS of exported `.htm` logs |
| `DL.Env(name, fallback)` | read an environment variable |
| `DL.SaveLog(path)` | dump the communication window so far to a file |
| `DL.HexToStr(h)` / `DL.StrToHex(s)` | byte↔string (1:1, Latin-1) |
| `DL.HexToDec(h)` / `DL.DecToHex(d, digits)` | number conversions |
| `DL.Asc(ch)` / `DL.Chr(n)` | character code helpers |

## Headless mode

```sh
# Run a project's conversation for 10 seconds and print the traffic:
lucidlight run device.llproj --port-a /dev/ttyUSB0

# Full scripted test; exits 1 when the script fails:
lucidlight run device.llproj --script smoke.js --duration 60 --baud 9600

# Machine-readable run for CI and agents (NDJSON, ends with a result line):
lucidlight run device.llproj --script smoke.js --json --out capture.log

# Teach your coding agent the whole workflow:
lucidlight skill install
```

Stream semantics: receives consume what they match; anything before or after
the match stays in the buffer. Finish a matched line with
`DL.ReceiveLine(timeout)` before reading the next one.

## Agent skill

`lucidlight skill` prints a compact guide (project schema, DL API, socat
loopback recipe) for coding agents; `lucidlight skill install` installs it
into `~/.pi/agent/skills/lucidlight/`.

## Examples

`examples/` ships ready-made projects: a PTY ping/pong loop
(`pingpong.llproj`) and a Modbus RTU master/slave pair
(`modbus-master.llproj`, `modbus-slave.llproj`) — see `examples/README.md`
for the socat recipes.

## Development

```sh
./build.sh          # qmake6 + make into build/lucidlight
./test.sh           # 105 unit and end-to-end tests (offscreen)
./asan.sh           # AddressSanitizer build, on demand (build/asan/)
tests/grab_harness   # LUCID_SIM=1 LUCID_TAB=script ./grab_harness — screenshots offscreen
```

Layout: `src/` C++ core, `src/qml/` UI, `tests/` suite + screenshot harness,
`docs/` (parity checklist, man page, agent skill), `examples/` sample
projects, `pkgbuild/` Arch packaging. One `.pro` file drives everything;
no build system beyond qmake6 + make.

## Continuous integration

Every push runs the full 105-test suite on GitHub Actions (Ubuntu 24.04,
Qt 6): `.github/workflows/ci.yml` — the CI has already caught two
portability bugs (non-standard baud constants, newer-Qt API) that Arch
machines could not.

## License

MIT — see [LICENSE](LICENSE).
