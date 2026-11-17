# Interactive Music Radio — design notes (2.2)

## What it is (and is not)

IMR is a **remote control for your music**, not a re-streamer. It links the game
to a music app or a music site that is already playing and makes that audio the
thing you hear in Skyrim — but it never captures audio and never plays it through
Skyrim's XAudio2 graph.

Why that matters: capturing would mean per-app protocols (DRM, browsers, rhythm
games all differ), buffering latency, and audio-graph plumbing. Observing the
machine's **audio sessions** instead is one well-documented Win32 API
(`IAudioSessionManager2`) and gives near-zero latency.

2.0 also dropped "list every session that makes noise". The catalogue is six
entries, and nothing else is ever shown — a decision made once, in
`sources.cpp`, not scattered through the UI. 2.2 removed the UI Automation
layer entirely: the four buttons and the input field it served never worked
reliably, so no code walks another program's accessibility tree any more. The
source catalogue, command protocol, build link libraries and front-end were
cleaned of every trace.

## Components

```
main.cpp        SKSE lifecycle, event-woken 16 ms engine thread, snapshot
                publish, recorded-hotkey input sink
platform.cpp    Data dir / plugin dir resolution (no absolute paths)
sources.cpp     the six-entry catalogue + program classification (pure)
session.cpp     AudioSessionController: audio sessions (volume / running state)
nowplaying.cpp  NowPlaying: OS media sessions (SMTC) - metadata + transport,
                play-mode switches, seek; all async calls time-bounded
lyrics.cpp      LyricsOverlay x2: layered, draggable / lockable lyric windows
lyrics_net.cpp  lyric look-up over WinHTTP (the only networked code)
lrc.cpp         LRC parsing + "which line is playing at N" (pure)
handshake.cpp   CmsHandshake: 64-byte block with a heartbeat, read by CMS
ui.cpp          PrismaUI bridge: RequestPluginAPI -> CreateView / InteropCall /
                RegisterJSListener; command queue drained by the engine thread;
                wake-notifier + identical-payload suppression
core.h/.cpp     Config, Snapshot, MediaSource, NextLinkState, PlayModeOf(Loose),
                PositionNow, logger
ui/             front-end (HTML/CSS/JS), deployed to Data/PrismaUI/views/
                InteractiveMusicRadio/ - that is the only base PrismaUI
                resolves file:///views/ against (1.1.1)
extern/         PrismaUI_API.h, copied verbatim from the framework release
```

## The three threads

1. **Game thread** — owns every `RE::*` call: the input sink decides when the
   radio opens or closes, and queued tasks apply PrismaUI `Show`/`Hide`. It
   never polls audio or COM.
2. **Engine thread** — owns every COM/WinRT call. It initialises its own MTA
   apartment here, polls the audio sessions and the OS media sessions, drives
   `CmsHandshake::SetTakeover`, and pumps the lyric windows' messages. No
   `RE::*` here. A `condition_variable` wakes it the instant a JS command
   arrives, so a click does not wait for the next frame; the 16 ms frame keeps
   the lyric clock and the progress bar smooth.
3. **PrismaUI (its own main-thread queue, 1.5.1+)** — renders the snapshot.
   JS calls `window.imrCommand(text)`, which may land on a foreign thread; the
   callback therefore only appends to a bounded queue (64 entries) and signals
   the engine. `Tick()` drains the queue *before* repainting. Outbound pushes
   use `InteropCall(view, "imrUpdate", json)` — one string argument, never
   generated JavaScript source, and skipped when the serialised payload is
   byte-for-byte identical to the previous one.

The snapshot crosses the game/engine boundary with a seqlock, exactly like
CMS's `GameContext`.

## Lifecycle guarantees

* **Exit → disconnect**: `StopAll()` clears the handshake word and calls
  `RestoreAll()`, returning every touched session to its previous volume/mute.
* **Cross-save stable**: the link state lives only in memory (shared block +
  process state). Nothing is written into a save file, so loading another save
  cannot break the link.
* **No symbol clash with CMS**: own `imr::` namespace, own logging macros, own
  DLL / log / INI names.

## CMS handshake (non-invasive)

IMR never patches CMS and never links against it. The two meet in a single
64-byte shared page, `IMR_CMS_HANDSHAKE` (layout: `imr::ImrBlock` /
`cms::ImrLink`, both `static_assert`ed to 64 bytes):

```
u32 magic 'IMRH' | u32 version | u32 state | u32 pid
u64 heartbeatMs  | u32 flags   | u32 pad[9]
```

* `state = 1` — the radio is on the air; CMS holds its local music layer down
  and keeps the vanilla category at 0.
* `state = 0` — idle/gone; CMS owns the context again.

**The heartbeat is what makes this safe.** A bare state word would survive a
crashed writer and leave the player in permanent silence.  IMR refreshes
`heartbeatMs` on every `SetTakeover()` (each engine tick); CMS ignores a block
older than 3 s, so a force-killed IMR releases the music by itself.  CMS also
treats a stamp *ahead* of its own tick as stale — that is a block left over from
a previous boot, where `GetTickCount64()` restarted lower.

CMS 2.6.6 reads this block; a CMS without the reader still works — IMR just
cannot duck the local layer and remains a passive session inspector.

## Where the data comes from

| Need | API | File |
|---|---|---|
| is the app running, its volume / mute | `IAudioSessionManager2` | `session.cpp` |
| 歌名 / 专辑 / 歌手, play/pause/next/prev, position | `GlobalSystemMediaTransportControlsSessionManager` (C++/WinRT) | `nowplaying.cpp` |
| 随机 / 顺序 / 单曲 / 列表 | `IsShuffleActive` + `AutoRepeatMode` (SMTC), set through `TryChangeShuffleActiveAsync` / `TryChangeAutoRepeatModeAsync` | `nowplaying.cpp`, `core.cpp::PlayModeOfLoose` |
| 进度跳转 | `TryChangePlaybackPositionAsync` (100 ns ticks) | `nowplaying.cpp::Seek` |
| 精确歌词位置 | `TimelineProperties.LastUpdatedTime` + steady-clock interpolation | `nowplaying.cpp`, `core.cpp::PositionNow`, `main.cpp::EffectivePosition` |
| lyrics | 网易云 public lyric API over WinHTTP + LRC | `lyrics_net.cpp`, `lrc.cpp` |

SMTC is the only *sanctioned* way to read and drive a minimised player. Every
one of its `IAsyncOperation` calls is awaited with a finite timeout (60 ms for
metadata, 400 ms for commands); an unresponsive app degrades that one sample
instead of stalling the engine loop. Only sessions that classify to one of the
six catalogue entries get the expensive COM/WinRT work.

## Config

`InteractiveMusicRadio.ini` (next to the DLL): `bEnabled`, `bDuckInDialogue`,
`fDialogueDuck`, `fLocalDuck`, `iSessionPollMs`, `iTrackPollMs`,
`iHotkeyKey` / `iHotkeyMods` (recorded in-game, DirectInput **scan codes** —
`InputEvent::GetIDCode()` is not a VK), `iPanelX` / `iPanelY`,
`iLyricDesktopX/Y` / `iLyricWebX/Y`, `bLyricDesktopLocked` / `bLyricWebLocked`,
`bLyricsOnStart`, `bFetchLyrics`, `iLyricFontSize`, `iLyricOffsetMs`,
`bVerbose`. `iOpenKey` / `bCtrlInSystemMenu` from old files are read only for
backwards compatibility and otherwise ignored.

## The hotkey

2.1 replaced the system-menu binding with a hotkey recorded in-game.  The
player records a key in the panel (`hotkey act=capture`) and it is written as a
DirectInput scan code plus a modifier mask.  Details worth keeping:

* capture runs on the engine thread with a precomputed
  `MapVirtualKeyW(MAPVK_VSC_TO_VK)` table + `GetAsyncKeyState`, NOT the SKSE
  event sink: PrismaUI's `Focus()` can hold keyboard focus and the sink would
  miss the very key being recorded.  **Esc** cancels, `↺` restores F10, and a
  350 ms guard skips keys already held;
* the launcher is detected by BOTH the sink and the async-key poller.  A 300 ms
  `TryToggle()` dedup makes exactly one win, so it works whether or not the view
  has focus.  Console / book / race menus still block it;
* the view is created **hidden** (`CreateView` shows by default - 2.0.0 forgot
  that and dropped a full-screen card over the main menu, hiding the Load
  button). `Attach()` hides it and `OnDomReady` re-asserts the state, because
  PrismaUI shows a freshly loaded page;
* `Show`/`Hide` are renderer calls and only work on the game thread. The engine
  thread queues them through `ui::SetMainThreadRunner` (installed by `main.cpp`,
  backed by `SKSE::GetTaskInterface`) - direct calls from the engine thread
  silently did nothing, which is why the ✕ button was dead in 2.0.0;
* `g_want` (intent, any thread) is separated from `g_applied` (what PrismaUI is
  showing, game thread only) so `ui::Shown()` is truthful;
* **Escape** is a hard-wired dismiss, so "the panel is up and I cannot reach
  Load" is not a possible state.

## Seek, progress bar and lyric sync (2.2)

`core.cpp::PositionNow()` is the one authoritative interpolation: it takes an
SMTC sample (already corrected by `TimelineProperties.LastUpdatedTime`),
extrapolates it against the shared steady clock while playing, and clamps it
to the track duration. `main.cpp::EffectivePosition()` wraps it with a seek
override and feeds **both** the published `sources[].track.position` and the
lyric highlight, so the bar and the words can never disagree.

1. The front-end computes a target second from the drop point and sends
   `seek\npos=…`; it sets a `pendingSeek` guard so the next snapshot cannot
   flash the thumb back to the old value.
2. C++ calls `TryChangePlaybackPositionAsync`, then anchors `g_seekPos` /
   `g_seekAt`, rewrites its cached media timeline and the live snapshot, and
   issues `play` if the player was paused — playback continues from the chosen
   point.
3. The override runs forward with wall time and is released as soon as the
   player reports a position within 1.5 s of it (or after 3 s). The front-end
   drops its guard on the same condition.
4. `歌词校准 ±250 ms` remains the only extra term, applied to the lyric clock
   alone.

Desktop and web players use **separate** `LyricsOverlay` + `LyricDeck` pairs
(`g_lyrics[2]`, `g_decks[2]`): only the one matching the linked kind is shown,
but each keeps its own position, lock state and parsed lyric. Unlocking removes
`WS_EX_TRANSPARENT` and lets `WndProc` drag the layered window; the position is
saved to the INI on mouse-up.

## Four play modes (2.2)

`PlayModeSwitches()` maps the four rows onto the two SMTC switches; shuffle also
asks for list repeat so a shuffled queue keeps playing. `PlayModeOfLoose()` is
the *display* inference: many players publish only one of the switches, and the
strict `PlayModeOf()` used to show "no mode" for them.

A `playmode` command writes an optimistic override (`g_modeOverride`) that no
poll may overwrite, publishes it to the panel immediately, and calls
`SetPlayMode()`. The override is cleared only when the player reports the same
mode back. When the session does not exist yet, or the player refuses, a
`PendingMode` re-applies up to three times; after three refusals a strictly
guarded `LocalMode` enforces the order at the end of a track (seek to 0 for
单曲循环, SkipNext for 列表循环 / 随机播放). 顺序播放 never intervenes, and a
normal player that accepts the switches is never touched. Invalid mode values
and a source switch clear all pending state.

## Responsiveness (2.2)

* JS commands signal `g_wakeCv` from the PrismaUI thread; the engine wakes from
  its 16 ms `wait_for` immediately and drains commands before repainting.
* `PushNow()` compares the JSON with the previous push and skips identical
  frames; periodic pushes run at 10 Hz instead of 4 Hz.
* `radio.js` rebuilds the two source lists, the now-playing row and the
  settings row only when their *signature* changes; per-frame work is reduced
  to two style writes and the time labels.
* Audio-session enumeration skips sessions that cannot classify to a catalogue
  entry before doing COM `QueryInterface` work; media metadata reads are capped
  at 60 ms each.
* All WinRT `IAsyncOperation::get()` calls were replaced by bounded
  `wait_for` + `GetResults()` (60 ms metadata, 400 ms commands), so a hung
  player cannot freeze the handshake, the lyric windows or the UI.

## When the view is blank (1.1.1)

Three things were added after 1.1.0 shipped a view that never rendered:

* **Correct root.** PrismaUI builds `file:///views/<path>` and resolves it under
  `Data\PrismaUI\views\`. The 1.1.0 layout (`Data\InteractiveMusicRadio\`) never
  loaded; PrismaUI still handed back a valid view, so nothing errored — the
  screen just stayed empty.
* **JS console relay (V2).** IMR asks for `IVPrismaUI2` first and registers
  `RegisterConsoleCallback`, so `js[error] …` lines land in the plugin log.
  On a V1-only build the radio falls back to `IVPrismaUI1` and works unchanged.
* **DOM watchdog.** If the DOM is not ready 5 s after `CreateView`, the log
  states the required path once, instead of leaving the user with silence.

## Limits

* Only sees programs attached to the **default render endpoint**.
* External audio is not captured; the "in-game volume control" of a source is
  really a per-session volume control on the shared endpoint.
* 网页版 shares one browser media session across the three cards - Windows does
  not expose which site a tab is playing.
* Lyrics come from an online service; offline, the overlay shows the track line.
* SMTC has no "pick a random track" call; on a player that refuses the mode
  switches, 随机播放 follows that player's own queue.
* The PrismaUI view is optional: IMR loads and its media-control features work
  even without PrismaUI, only the visual radio is skipped.
