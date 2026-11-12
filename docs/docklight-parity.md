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
- [~] Named pipes — planned as Unix domain sockets / FIFOs
- [ ] Side channels (up to 8 extra connections, DL.OpenSideChannel)

## Serial settings
- [x] Baud 300..4000000, data bits 5-8, parity none/odd/even/mark/space,
      stop bits 1/1.5/2, flow off/RTSCTS/XONXOFF, RTS toggle/DTR toggle,
      break condition
- [ ] Parity error replacement character (ParityErrorChar, default 63)

## Communication window
- [x] Four representations: ASCII/HEX/DEC/Binary (Binary also parses for
      sending, 8-bit groups)
- [x] Tabbed representations with Ctrl+Tab / Ctrl+Shift+Tab cycling
- [x] Control character names (<CR><LF><NUL>...) in ASCII view, toolbar toggle
- [x] Contents filter: all / channel 1 / channel 2 / comments only
      (+ DL.SetContentsFilter via bridge)
- [~] Channel 1 / Channel 2 color+style distinction (RXA/TXA/RXB pills)
- [x] Date/time stamps with ms precision, direction-change stamps
- [x] Delta time stamps
- [ ] Clock stamps at fixed interval (Options: "additional date/time stamp")
- [x] Pause detection stamps after configurable silent gap (1 s toolbar
      toggle; the delta column restarts after the gap)
- [x] Clear log, freeze/log search (Lucidlight extension)
- [~] Find sequence in communication window — live filter exists (Ctrl+F);
      Docklight-style find-dialog navigation missing
- [ ] Copy/paste per row exists via context menu; full-window print missing

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
- [~] Sequence documentation text — comment column exists in the table
- [~] 64-sequence capacity, sequence index display (0-based)

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
- [~] DL.SetUserOutput (signal exists; script output pane shows it as lines)
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

## VBScript basics (JS equivalents)
- [x] Conversions Asc/Chr/HexToStr/StrToHex/HexToDec/DecToHex
- [x] #include directive (script-side file inclusion)
- [x] vbCrLf / vbOK / vbYes / MsgBox-button constants predefined

## Program options / project
- [x] Project save/load (.llproj ≈ .ptp)
- [x] Session restore, unsaved-changes guard
- [x] Startup script auto-run (project option, "Run on open"; headless runner
      executes the project script when no --script is given)
- [x] Program options save/load via DL.Load/SaveProgramOptions (JSON store)

## Hot keys (F1 help, Ctrl+N/O/S/P, F5/F6 comm, F2/F3 logging,
## Shift+F5/F6/F7 script, Ctrl+T save script, F12/Shift+F12 layout,
## Ctrl+F find, Ctrl+W clear, Ctrl+Tab representation, Space send, Ctrl+E edit)
- [x] F1-F12 sequence hotkeys, Space toggles comm, F5/F6 comm, F2/F3 file
      logging, Shift+F5/F6/F7 script run/stop/panel, Ctrl+T save script,
      Ctrl+F find, Ctrl+K clear, Ctrl+Tab representation cycling

## Scripting extras (Lucidlight extensions, documented as such)
- [x] DL.Send/SendHex/SendStr/SendBytes, DL.Receive/ReceiveStr/ReceiveHex/
      ReceiveLine, DL.Assert/DL.Fail, DL.Env, DL.SaveLog, DL.TimeDiff
- [x] Headless runner: `lucidlight run` with --json NDJSON, exit codes
- [x] Agent skill: `lucidlight skill [install]`
- [x] Blocking-call parking: queued DL_OnReceive events run while a script
      waits, mirroring Docklight's event timing

## HTML output / misc
- [n/a] Docklight HTML output customization, window layout themes
