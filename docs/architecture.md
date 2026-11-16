# Lucidlight architecture

A map of the codebase for maintainers. Read top-down; each layer only
depends on the ones above it.

```
                 ┌─────────────────────────────┐
   QML (src/qml) │ Main.qml + panels/dialogs   │  UI only; no protocol code
                 └──────────────┬──────────────┘
                                │ context properties: backend, theme, translator
                 ┌──────────────▼──────────────┐
   application   │ Backend (src/backend.*)     │  state, session, bridges
                 │  ├ ScriptRunner(s)          │  script threads + dispatch pool
                 │  ├ CommManager              │  owns the links
                 │  ├ CommLog / FileLogger     │  window rows / file output
                 │  └ SequenceModel            │  the sequence list
                 └───────┬──────────────┬──────┘
                         │              │
        communication    │              │  scripting
                 ┌───────▼──────┐  ┌────▼──────────────────────┐
                 │ CommLink     │  │ DLScriptEngine + DLObject │
                 │ ├ SerialLink │  │ (src/dlscript.*,          │
                 │ └ NetLink    │  │  src/vbstrans.*)          │
                 └──────────────┘  └───────────────────────────┘
   supporting: DataFormat, Checksum, SequenceMatcher, Decoder,
               Theme, Translator, PortalPicker, PortEnumerator,
               JsHighlighter, Project (load/save)
```

## Communication layer

- **`CommLink`** (abstract): one channel. `create(device)` picks the
  implementation; `normalizeDevice()` is the *single* point where device
  strings are classified (never split scheme prefixes at the first colon).
- **`SerialLink`**: POSIX termios directly (no Qt SerialPort dependency).
  Read loop starts at 8 KiB and doubles the buffer until drained, so burst
  size is bounded only by RAM. BREAK is detected via PARMRK (`FF 00 00`
  triples in the byte stream). Records its device string even when the open
  fails (error reporting + auto-reopen depend on it).
- **`NetLink`**: TCP client/server, UDP (sender/receiver), UNIX stream
  client/server. Connects are fully asynchronous — synchronous
  `waitForConnected()` disables Qt's internal read notifier when the peer
  accepts later. One UDP server answers whoever last sent.
- **`CommManager`**: owns both main links and side channels 3–10 (Docklight
  numbering). Forwards RX→TX in Redirect and Monitor (Data Forwarding),
  emits `incoming` for the script engines, `sendOnEx` for all writes,
  `reopenChannel` for the auto-reopen feature.

## Logging

- **`CommLog`** (`QAbstractListModel`): the window. Rows carry direction,
  channel, raw bytes, µs stamps (`CLOCK_REALTIME`, `clock_gettime` —
  QDateTime only resolves ms) and µs deltas. Frozen mode buffers rows for
  the Snapshot reveal. Rows trim to the newest 5 000 (oldest 1 000 dropped
  with an event row), so RAM stays bounded.
- **`FileLogger`**: live `.log/.txt` (Docklight text format), `.htm`
  (full HTML, CSS from `DL.SetHtmlStyle`), `.rtf`, `.md` (Markdown table),
  `.csv` (RFC 4180). Multi-representation sets (`A/H/D/B` suffix files).
  `writeSnapshot` replays buffered window rows for Save-Log/`DL.SaveLog`.
  Header/footer say "Lucidlight Log File"; CSV files are strictly tabular.

## Scripting

- **`DLScriptEngine`**: one `QJSEngine` per script, moved to its own
  `QThread`. Incoming data and state arrive via queued connections; the
  engine parks blocking `DL.Receive*` calls in a local `QEventLoop` with
  50 ms slices, so the UI never blocks and Stop (`QJSEngine::
  setInterrupted`) always wins.
- **`DLObject`**: the `DL.*` API surface (200+ calls, Docklight-compatible
  semantics). Calls that need the main thread go through the
  **backend bridge**: `Backend::scriptInvoke` via
  `Qt::BlockingQueuedConnection`. Blocking calls pass `drainDuring=false`
  — receive events queue and drain once after the call returns.
- **`ScriptRunner`**: wires engine ↔ backend, owns the thread, stop with
  bounded join. **Dispatch**: `Backend::scriptDispatch` creates additional
  runners (max 8); `stopDispatches()` stops them *and waits*, because
  destroying a runner whose `QThread` still runs aborts the process.
- **`vbstrans`**: transpiles classic Docklight VBScript to JS on the fly
  (`With` blocks, `Select Case`, single-line If/Else, line continuations,
  `&` precedence, `DL_OnReceive` semantics). `.js` files are never
  misdetected as VBScript.

## Sequences and checksums

- **`SequenceModel`**: send/receive sequences with wildcard parameters,
  hotkeys, repeat, reply-to chains, checksum specs, trigger actions.
  **`SequenceProxy`** is the panel's grouped view (sends first, receives
  second) — a plain mapping model with a queued rebuild; the source order
  stays the scripting index.
- **`SequenceMatcher`**: streaming receive-pattern matcher per channel
  (`?` any byte, `#` digit runs, function characters), fires triggers
  (comment, checksum validation, snapshot, stop-run) and answers send
  sequences.
- **`Checksum`**: parses the manual's spec grammar
  (`MOD256 | XOR | LRC | LRC-ASCII | CRC-* | CRC:width,poly,init,xor,ri,ro`
  with `(start,len)` areas, `L` little-endian, `@target` placement) and
  computes. Note: results are MSB-first by default like Docklight's
  CalcChecksum; real Modbus wire order needs `CRC-MODBUS L`.
- **`Decoder`**: `ModbusDecoder::tryDecode` — validates CRC-16/MODBUS and
  renders a one-line annotation for requests, responses and exception
  frames. Hooked in `Backend::handleIncoming`; toggle via Expert Options or
  `DL.SetModbusDecode`.

## Application plumbing

- **`Backend`**: the god-object surface QML talks to. Owns session
  persistence (`~/.local/state/lucidlight/session.json`: channels,
  sequences, script, recent files, geometry, expert options), headless
  bridges, snapshot state machine, auto-reopen timer (2 s poll of closed
  main channels), and the external-editor temp file + `QFileSystemWatcher`
  reload (file created *before* `addPath` — the watcher fails silently on
  nonexistent paths).
- **`main.cpp`**: GUI mode (QML engine, theme, translator) vs `run`
  headless mode (plain `QCoreApplication`, NDJSON `--json`, `--duration`
  kill switch, `--out` log dump). `skill` serves the agent guide.
- **`Theme`**: reads the desktop color scheme (Omarchy/Hyprland with
  graceful fallbacks); `Translator` reloads language live.
- **`PortalPicker`**: file dialogs through xdg-desktop-portal (no native
  dialog dependency).

## Invariants worth knowing before changing anything

1. The relative µs clock baseline resets **only** in `Backend::toggleComm`
   — TCP servers fire `runningChanged` on connect and must not re-zero it.
2. `QByteArray(const char*)` truncates at embedded NULs — build byte arrays
   with `append()` when protocol bytes may contain 0x00.
3. Tests exercise `toggleComm()` (not `start()`) when repeat timers or
   triggers must run; `LUCID_PARAMS`/`LUCID_MSGBOX` auto-answer dialogs
   headlessly.
4. After adding test slots or QML resources, re-run `qmake6` in
   `build/tests` — a plain `make` can link stale moc output.
5. Blocking DL calls must use the bridge (never touch the GUI thread
   directly); the bridge is the only thread boundary.
