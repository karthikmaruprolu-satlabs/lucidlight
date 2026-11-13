# Docklight Scripting parity checklist

Source of truth: the official Docklight Scripting V2.4 user manual
(https://docklight.de/manual/), mirrored in /tmp/dldoc during development.
Each item maps a Docklight feature to its Lucidlight counterpart. Linux
substitutions are noted where the Windows original cannot be replicated
one-to-one (COM ports become /dev/tty*; Docklight TAP hardware does not exist).

Status: [x] done, [~] partial, [ ] missing, [n/a] not applicable on Linux.

## Communication channels (Project Settings: Communication)
- [x] Communication Mode: Send/Receive (Single) — RX/TX on one port
- [x] Communication Mode: Redirect (Docklight "Send/Receive with 2 channels")
- [x] Communication Mode: Monitoring (2 ports, receive only, data forwarding)
- [~] Serial COM ports — /dev/tty* via termios, incl. 9 data bits oddities
- [x] TCP client (`TCP:host:port`), TCP server (`SERVER:port`, status 2 while
      listening). Docklight's TCP PROXY is monitoring mode with two TCP
      endpoints, which Monitor mode already provides.
- [x] UDP peer (`UDP:host:port[:localPort]` with split ports), UDP server
      (`UDP:port` binds, answers the last sender)
- [n/a] Docklight TAP / VTP hardware taps (Windows-only hardware)
- [n/a] USB HID (needs hidapi; not installed)
- [x] Named pipes — Unix domain sockets: `UNIX:/path` (client) and `UNIXSERVER:/path` (single-connection listener), including DL/scripts and side channels

## Serial settings
- [x] Baud 300..4000000, data bits 5-8, parity none/odd/even/mark/space,
      stop bits 1/1.5/2, flow off/RTSCTS/XONXOFF, RTS toggle/DTR toggle,
      break condition
- [x] Parity error replacement character (ParityErrorChar, default 63) — termios PARMRK decoding; settings UI field, project persistence, DL channel-settings string field 6, "(ignore)" keeps the raw triplet

## Communication window
- [x] Four representations: ASCII/HEX/DEC/Binary (Binary also parses for
      sending, 8-bit groups)
- [x] Tabbed representations with Ctrl+Tab / Ctrl+Shift+Tab cycling
- [x] Control character names (<CR><LF><NUL>...) in ASCII view, toolbar toggle
- [x] Contents filter: all / channel 1 / channel 2 / comments only
      (+ DL.SetContentsFilter via bridge)
- [x] Channel 1 / Channel 2 color distinction (RXA/TXA/RXB pills): RX1 green,
      RX2 cyan, TX1 blue, TX2 magenta, both badge and data text
- [x] Date/time stamps with ms precision, direction-change stamps
- [x] Delta time stamps
- [x] Clock stamps at fixed interval — ⏲ toolbar menu (off/1/5/10/60 s), `backend.clockStampInterval`, full date+time event rows while running
- [x] Pause detection stamps after configurable silent gap (1 s toolbar
      toggle; the delta column restarts after the gap)
- [x] Clear log, freeze/log search (Lucidlight extension)
- [x] Find in communication window — Ctrl+F field with live highlight + Enter/"Next" jumps row to row (wraps), found row highlighted
- [x] Copy/paste per row via context menu; full-window print → Ctrl+P renders the whole window into a PDF (QPdfWriter)

## Sequences
- [x] Send sequences: name, data, hotkeys F1-F12, repeat periodically
- [x] Receive sequences: name, data, trigger auto-send
- [x] '#' wildcard (zero or one char, greedy with backtracking) and
      '?' single-char wildcard
- [x] Send sequence wildcard parameters (dialog + DL.SendSequence parameters)
- [x] Special function characters: '&' inter-char delay, '%' break state,
      '!' handshake signal bits (send + receive side)
- [x] Receive sequence actions: Answer / Comment (macros) / Trigger
      (snapshot) / Stop / Checksum
- [x] Receive sequence comment macros %_S %_L %_T %_C %_X %_I %_N %_A %_H
      %_D %_B incl. extended (start,len) syntax
- [x] Per-sequence hit counters (DL.GetReceiveCounter / ResetReceiveCounter)
- [x] Checksum areas in send sequences (auto-calc per Checksum Specification)
- [x] Checksum validation in receive sequences (OK / WRONG / BOTH actions)
- [x] Sequence documentation text — comment column + row tooltip in the table
- [x] Sequence capacity/index display (0-based badges; capacity unbounded —
      exceeds Docklight's 64)

## Checksums / CRC (Checksum Specification)
- [x] MOD256, XOR, MOD65536, LRC (-MOD256), LRC-ASCII
- [x] CRC-7, CRC-8, CRC-DOW, CRC-CCITT, CRC-XMODEM, CRC-16, CRC-MODBUS, CRC-32
- [x] Generic CRC:width,polynomial,init,finalXOR,reflectedIn,reflectedOut
- [x] DL.CalcChecksum with startPos/endPos (negative ok) and endianness

## Logging
- [x] Start/Stop Communication Logging with _asc/_hex/_dec/_bin files
- [x] Formats: plain text (.txt), HTML (.htm), RTF (.rtf)
- [x] Append/overwrite, headers on/off (DL.StartLogging arguments)

## Scripting (DL object)
Docklight uses VBScript; Lucidlight uses JavaScript (QJSEngine) — scripts are
not source-compatible, but the DL API names and semantics are replicated.

### Methods (basic)
- [x] DL.StartCommunication / StopCommunication / DL.ClearCommWindows*
- [x] DL.SendSequence nameOrIndex [, parameters] [, representation]
      (wildcard areas filled Docklight-style — ASCII chars fill the area and
      pad with blanks, comma-separated values for H/D/B; DL_OnSend data
      manipulation before the bytes hit the line)
- [x] DL.WaitForSequence(nameOrIndex [, maxCounter] [, timeout]) → counter
      (absolute hit counter since comm start / last reset, like Docklight;
      also matches against the engine's local stream so quick answers are
      never missed)
- [x] DL.AddComment (timeStampAfterComment, lineBreakAndPadding)
- [x] DL.GetDocklightTimeStamp ([dateTime] [, millis] [, trimmed])
- [x] DL.Pause + DL.Wait alias
- [x] DL.GetReceiveCounter(nameOrIndex) / DL.ResetReceiveCounter([nameOrIndex])
- [x] DL.OpenProject / DL.Quit
- [x] DL.StartLogging / DL.StopLogging
- [x] Properties DL.NoOfSendSequences / DL.NoOfReceiveSequences

### Methods (advanced)
- [x] DL.CalcChecksum
- [x] DL.ConvertSequenceData (toSingle/fromSingle/toDouble/fromDouble,
      fromText, toText, to/from Integer16/32, Unsigned16/32, toBool)
- [x] DL.GetChannelSettings / DL.SetChannelSettings ("COMx:9600,N,8,1" syntax
      with ">" port search; TCP/UDP strings when those channels land)
- [x] DL.GetChannelStatus (0 closed / 1 open / 2 waiting / 3 error)
- [x] DL.GetCommWindowData("A|H|D|B")
- [x] DL.GetEnvironment (+ DOCKLIGHT_* equivalents as LUCIDLIGHT_*)
- [x] DL.GetHandshakeSignals / DL.SetHandshakeSignals (bit-coded CTS/DSR/DCD/RI)
- [~] DL.GetKeyState (keyboard modifiers only, documented deviation)
- [x] DL.GetReceiveComments
- [x] DL.InputBox2 / DL.MsgBox2 (QML dialogs; headless: env-driven answers)
- [x] DL.LoadProgramOptions / DL.SaveProgramOptions
- [x] DL.PlaybackLogFile
- [x] DL.SetContentsFilter (bridge → communication window)
- [x] DL.SetUserOutput (script output pane shows the lines)
- [x] DL.SetWindowLayout ("S", "D", "SD", "")
- [x] DL.ShellRun (QProcess-based)
- [x] DL.UploadFile (A/H/D/B/R representations)
- [x] DL.DirectSend (channel injection without window output)

### Event procedures
- [x] Sub DL_OnSend() with OnSend_GetSize/GetName/GetIndex/GetData/SetData/
      Peek/Poke
- [x] Sub DL_OnReceive() with OnReceive_GetSize/GetName/GetIndex/GetData/
      Peek/GetChannel/GetDateTime/GetMilliseconds

### FileInput / FileOutput objects
- [x] FileInput: read char/line, EOF, Close
- [x] FileOutput: create/append, text + binary, Close

### Side channels
- [x] DL.OpenSideChannel / DL.CloseSideChannel / DL.DirectSend channelNo 3..10
      (serial or network devices; side-channel data flows into the incoming
      path with channel numbers 2..9)

### Debug object (manual "Script Debugging")
- [x] Debug.Mode = 0/1, Debug.PrintMsg (stamped debug line in the comm
      window), Debug.Assert (stops the script when false — Docklight waits
      for a manual Continue; Lucidlight ends the run, documented deviation),
      Debug.Break

## VBScript basics (JS equivalents)
- [x] VBScript/JavaScript auto-detection: clear JS constructs (var/;/{/)
      veto the VBScript translation
- [x] Conversions Asc/Chr/HexToStr/StrToHex/HexToDec/DecToHex
- [x] #include directive (script-side file inclusion)
- [x] vbCrLf / vbOK / vbYes / MsgBox-button constants predefined

## Program options / project
- [x] Project save/load (.llproj ≈ .ptp)
- [x] Session restore, unsaved-changes guard
- [x] Startup script auto-run (project option, "Run on open"; headless runner
      executes the project script when no --script is given)
- [x] Program options save/load via DL.Load/SaveProgramOptions (JSON store)

## Hot keys — per the manual's "Hot Keys" appendix
- [x] F1 context help (also "?"), F5 start / F6 stop communication, F2/F3
      file logging, Ctrl+F5/F6 keyboard console on/off, F12 script area,
      Shift+F12 sequence lists, Shift+F5 run / Shift+F6 stop script,
      Ctrl+T save script, Ctrl+F find, Ctrl+W clear, Ctrl+Tab
      representation cycling, Space send
- [x] Sequence hotkeys on Ctrl+F1..Ctrl+F12 (Lucidlight extension; Docklight
      has no sequence hotkeys — the plain F-keys are reserved)

## Scripting extras (Lucidlight extensions, documented as such)
- [x] DL.Send/SendHex/SendStr/SendBytes, DL.Receive/ReceiveStr/ReceiveHex/
      ReceiveLine, DL.Assert/DL.Fail, DL.Env, DL.SaveLog, DL.TimeDiff
- [x] Headless runner: `lucidlight run` with --json NDJSON, exit codes
- [x] Agent skill: `lucidlight skill [install]`
- [x] Blocking-call parking: DL_OnReceive events detected during Pause /
      WaitForSequence / Receive / SendSequence are queued and run after the
      call returns, exactly per the manual's "Timing and Program Flow"

## HTML output / misc
- [x] HTML log output customization — .htm log files are full documents
      with a `<style>` block; the CSS is set via the Lucidlight extension
      `DL.SetHtmlStyle(css)` / `DL.GetHtmlStyle()` (default monospace small).
      Window layout themes: covered by the Omarchy theme engine instead.

## Recently closed (0.2.x)
- [x] Microsecond timing — the µs toolbar toggle (and DL.SetMicroStamps /
      DL.GetMicroStamps) switches every time display to seconds-only with
      6 decimals: the clock column counts seconds from window start, the
      delta column shows us-accurate gaps between rows, and the log files /
      saved snapshots use the same format (CLOCK_REALTIME us precision)
- [x] Example projects under examples/ (ping-pong demo, Modbus RTU
      master/slave pair with CRC areas)
- [x] External editor (Customize / External Editor) — command template with
      %FILE%/%LINE% wildcards, temp script file, auto-reload on change via
      QFileSystemWatcher; menu entries "Edit script in external editor" and
      "Customize / External Editor…"
- [x] Save communication log in HTML and RTF — the save dialog accepts
      .htm/.html/.rtf in addition to plain text; the snapshot renders the
      window's current representation through the same HTML/RTF machinery as
      the streaming logger
- [x] Find Sequence — the search bar now parses the needle in the currently
      selected window representation (ASCII/HEX/DEC/Binary, wildcards and
      [0D]-style bytes included) and matches raw row bytes, like Docklight's
      Edit > Find Sequence in Communication Window
- [x] Snapshot — camera toolbar button: hides the communication window while
      armed (log frozen, comm auto-started); when a receive sequence with the
      Trigger option fires, further data is processed until the trigger sits
      roughly in the middle of the buffered window, then the window is
      revealed, an event notes the trigger, and communication stops
      (regression test snapshotCatchesTriggerSequence)
- [x] Data Forwarding (Monitor mode expert option) — RX on one port is re-transmitted on the opposite port's TX line, on by default, toggle in the settings popup, persisted in the project
- [x] Control character shortcuts in the sequence editor — Ctrl+Enter inserts <CR>, Ctrl+Shift+Enter inserts <LF>
- [x] #include directive — script preprocessor splices .pts/.js files, relative to the script's directory then cwd, cycle-guarded
- [x] Keyboard Console — Tools menu toggle (Ctrl+T), Send/Receive mode only; typed keys transmit directly, Enter = CR+LF, ESC = 0x1B, Ctrl+V sends clipboard (1024-char cap)
- [x] DL.SetWindowOutput — alias of SetUserOutput with full signature; RTF input stripped to plain text; \u0001CLEAR marker clears the output pane (also fixed a C++ escape bug that broke the marker)
- [x] Deep-audit round 4 (script-level verification against the manual):
  - MOD65536 returned one byte instead of two — `QByteArray{char,char}` resolves to the (size,fill) constructor in Qt 6.12, silently dropping the first byte; all eight predefined CRCs plus MOD256/XOR/LRC now verified against the manual's "123456789" check values
  - DL.ConvertSequenceData: VBScript-style float output ("1.234567", "9.87987987987E+53"), "B" representation no longer parsed as decimal, decimal results padded to three digits ("103 154 149 160 081 161 036 075"); every manual example now reproduced exactly (regression test)
  - DL.GetChannelStatus: 0 closed / 1 open / 2 waiting (TCP & UNIX servers and clients) / 3 channel error (bad COM port after SetChannelSettings); side channels 3-10 supported
  - DL.SetChannelSettings no longer splits network device strings ("TCP:host:port") at the first colon; a failing port change keeps the session alive in the error state like Docklight
  - Periodic send (Repeat) rebuilds the sequence with wildcard parameters and checksum areas on every transmission, exactly like a manual send; the sequence dialog now takes seconds (0.01-9999) like Docklight's "Send periodically"
