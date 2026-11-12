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
- [ ] TCP client (`host:port`), TCP server (`SERVER:port`), TCP PROXY
- [ ] UDP peer (`UDP:host:port`), UDP with split ports, UDP server (`UDP:port`)
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
- [~] Four representations: ASCII/HEX/DEC/Binary — HEX + ASCII exist; DEC
      exists as display format; Binary tab missing
- [ ] Tabbed representations with Ctrl+Tab cycling like Docklight
- [ ] Control character names (<CR><LF><NUL>...) in ASCII view
- [ ] Channel 1 / Channel 2 color+style distinction
- [~] Date/time stamps with ms precision, direction-change stamps
- [~] Delta time stamps
- [ ] Clock stamps at fixed interval (Options: "additional date/time stamp")
- [ ] Pause detection stamps after configurable silent gap
- [ ] Contents filter: show all / channel 1 / channel 2 / comments only
      (+ DL.SetContentsFilter)
- [x] Clear log, freeze/log search (Lucidlight extension)
- [ ] Find sequence in communication window (Ctrl+F)
- [ ] Copy/paste, print communication window

## Sequences
- [x] Send sequences: name, data, hotkeys F1-F12, repeat periodically
- [x] Receive sequences: name, data, wildcard ?? matching, trigger auto-send
- [ ] '#' wildcard (matches zero or one character), '?' single-char wildcard
- [ ] Send sequence wildcard parameters (Send Sequence Parameter dialog;
      DL.SendSequence nameOrIndex, parameters, representation)
- [ ] Special function characters: '&' inter-char delay, '%' break state,
      '!' handshake signal bits (send + receive side)
- [ ] Receive sequence actions: Answer / Comment (macros) / Trigger
      (snapshot) / Stop / Checksum
- [ ] Receive sequence comment macros %_S %_L %_T %_C %_X %_I %_N %_A %_H
      %_D %_B incl. extended (start,len) syntax
- [ ] Per-sequence hit counters (DL.GetReceiveCounter / ResetReceiveCounter)
- [ ] Checksum areas in send sequences (auto-calc per Checksum Specification)
- [ ] Checksum validation in receive sequences (OK / WRONG / BOTH actions)
- [ ] Sequence documentation text shown in the main window
- [ ] 64-sequence capacity, sequence index display (0-based)

## Checksums / CRC (Checksum Specification)
- [ ] MOD256, XOR, MOD65536, LRC (-MOD256), LRC-ASCII
- [ ] CRC-7, CRC-8, CRC-DOW, CRC-CCITT, CRC-XMODEM, CRC-16, CRC-MODBUS, CRC-32
- [ ] Generic CRC:width,polynomial,init,finalXOR,reflectedIn,reflectedOut
- [ ] DL.CalcChecksum with startPos/endPos (negative ok) and endianness

## Logging
- [ ] Start/Stop Communication Logging with _asc/_hex/_dec/_bin files
- [ ] Formats: plain text (.txt), HTML (.htm), RTF (.rtf)
- [ ] Append/overwrite, headers on/off (DL.StartLogging arguments)

## Scripting (DL object)
Docklight uses VBScript; Lucidlight uses JavaScript (QJSEngine) — scripts are
not source-compatible, but the DL API names and semantics are replicated.

### Methods (basic)
- [x] DL.StartCommunication / StopCommunication / DL.ClearCommWindows*
- [ ] DL.SendSequence nameOrIndex [, parameters] [, representation]
- [ ] DL.WaitForSequence(nameOrIndex [, maxCounter] [, timeout]) → counter
- [x] DL.AddComment (as DL.Comment) — needs timeStampAfterComment and
      lineBreakAndPadding arguments
- [ ] DL.GetDocklightTimeStamp ([dateTime] [, millis] [, trimmed])
- [x] DL.Pause (as DL.Wait) — rename/alias
- [ ] DL.GetReceiveCounter(nameOrIndex) / DL.ResetReceiveCounter([nameOrIndex])
- [ ] DL.OpenProject / DL.Quit
- [ ] DL.StartLogging / DL.StopLogging
- [ ] Properties DL.NoOfSendSequences / DL.NoOfReceiveSequences

### Methods (advanced)
- [ ] DL.CalcChecksum
- [ ] DL.ConvertSequenceData (toSingle/fromSingle/toDouble/fromDouble,
      fromText, toText, to/from Integer16/32, Unsigned16/32, toBool)
- [ ] DL.GetChannelSettings / DL.SetChannelSettings ("COMx:9600,N,8,1" syntax
      with ">" port search; TCP/UDP strings later)
- [ ] DL.GetChannelStatus (0 closed / 1 open / 2 waiting / 3 error)
- [ ] DL.GetCommWindowData("A|H|D|B")
- [ ] DL.GetEnvironment (+ DOCKLIGHT_* equivalents as LUCIDLIGHT_*)
- [ ] DL.GetHandshakeSignals / DL.SetHandshakeSignals (bit-coded CTS/DSR/DCD/RI)
- [ ] DL.GetKeyState (limited: keyboard modifiers only)
- [ ] DL.GetReceiveComments
- [ ] DL.InputBox2 / DL.MsgBox2 (QML dialogs; headless: env-driven answers)
- [ ] DL.LoadProgramOptions / DL.SaveProgramOptions
- [ ] DL.PlaybackLogFile
- [ ] DL.SetContentsFilter
- [ ] DL.SetUserOutput (script output pane as user tab)
- [ ] DL.SetWindowLayout ("S", "D", "SD", "")
- [ ] DL.ShellRun (QProcess-based)
- [ ] DL.UploadFile (A/H/D/B/R representations)
- [ ] DL.DirectSend (channel injection without window output)

### Event procedures
- [ ] Sub DL_OnSend() with OnSend_GetSize/GetName/GetIndex/GetData/SetData/
      Peek/Poke
- [ ] Sub DL_OnReceive() with OnReceive_GetSize/GetName/GetIndex/GetData/
      Peek/GetChannel/GetDateTime/GetMilliseconds

### FileInput / FileOutput objects
- [ ] FileInput: read char/line, EOF, Close
- [ ] FileOutput: create/append, text + binary, Close

### Side channels
- [ ] DL.OpenSideChannel / DL.CloseSideChannel / DL.DirectSend channelNo 3..10

## VBScript basics (JS equivalents)
- [x] Conversions Asc/Chr/HexToStr/StrToHex/HexToDec/DecToHex
- [x] #include directive (script-side file inclusion)

## Program options / project
- [x] Project save/load (.llproj ≈ .ptp)
- [x] Session restore, unsaved-changes guard
- [ ] Startup script auto-run (project option)
- [ ] Program options save/load (.xml equivalent)

## Hot keys (F1 help, Ctrl+N/O/S/P, F5/F6 comm, F2/F3 logging,
## Shift+F5/F6/F7 script, Ctrl+T save script, F12/Shift+F12 layout,
## Ctrl+F find, Ctrl+W clear, Ctrl+Tab representation, Space send, Ctrl+E edit)
- [~] F1-F12 sequence hotkeys, Space toggles comm (Docklight: Space sends in
      the sequence list), some shortcuts exist in the UI

## Scripting extras (Lucidlight extensions, documented as such)
- [x] DL.Send/SendHex/SendStr/SendBytes, DL.Receive/ReceiveStr/ReceiveHex/
      ReceiveLine, DL.Assert/DL.Fail, DL.Env, DL.SaveLog, DL.TimeDiff
- [x] Headless runner: `lucidlight run` with --json NDJSON, exit codes
- [x] Agent skill: `lucidlight skill [install]`

## HTML output / misc
- [n/a] Docklight HTML output customization, window layout themes
