# Interactive Music Radio (交互式音乐电台)

A Skyrim SE/AE SKSE plugin that turns the game into a **remote control for your
music**, with a modern web UI rendered by [PrismaUI](https://www.prismaui.dev/).

2.0 narrowed the scope on purpose: it no longer listens to "every program that
makes noise". It serves **two kinds of source only**:

| 种类 | 内容 |
|---|---|
| 桌面版 | QQ音乐 · 网易云音乐 · 汽水音乐（本机安装的程序） |
| 网页版 | 同一服务的网页播放器（跑在系统浏览器里） |

Built as a **sibling of `CustomMusicSystem.dll`** — own DLL name, own log, own
INI, own `imr::` namespace — so the two plugins load side by side without a
single symbol clash.

## What it does

* **A hotkey you choose.** The Ctrl-in-system-menu binding is gone. Click
  `⌨ 热键` in the panel, press any key (optionally with Ctrl / Shift / Alt), and
  it becomes the launcher; the choice is written to the INI and survives a
  restart. `↺` restores F10. **Esc** always closes the panel. The recording
  reads the physical keyboard through Win32, so it works even while the PrismaUI
  view holds focus.
* **Starts hidden.** The panel is created hidden and stays so until you press
  your hotkey - it never sits over the main menu or the Load button.
* **Draggable panel, accurate seek.** Drag the header to move the whole radio;
  the offset is saved. Click or drag the progress bar and *release* to jump:
  `TryChangePlaybackPositionAsync` gets the chosen second, playback resumes from
  there if it was paused, and the bar keeps running from that point instead of
  snapping back to a stale timeline. Current / total time on both sides.
* **Lists exactly six sources** — three desktop programs, three web players —
  left column desktop, right column web. Each card shows the running state and,
  when a track is playing, **歌名 · 歌手**.
* **Controls a minimised player.** Metadata and transport come from the OS media
  session (`GlobalSystemMediaTransportControlsSessionManager`, the same API the
  Windows volume flyout uses): play/pause, 上一曲/下一曲, plus 歌名 / 专辑名 /
  歌手名 — including for a player sitting in the tray.
* **随机播放 / 顺序播放 / 单曲循环 / 列表循环.** The same four modes, nothing
  added or renamed. They map to the media session's two switches
  (`IsShuffleActive` + `AutoRepeatMode`). A selection is shown at once and is
  re-applied until the player reports it back; a player publishing only one
  switch is inferred instead of showing "no mode". A player refusing both
  switches gets its order enforced by the radio at the end of each track.
* **Two independent desktop lyric windows**, one for a desktop player and one
  for a web player - each with its own lyric, position and lock state. Unlock in
  the panel and the window becomes draggable (teal grip bar + hint line); lock it
  again and it is click-through. The 桌面歌词 switch controls *only* our windows,
  never another program's lyric widget.
* **Lyrics that stay on the beat.** Playback position is corrected by SMTC's
  `LastUpdatedTime` and interpolated against a shared steady clock; the *same*
  value drives the progress bar and the lyric highlight, so they cannot
  disagree. A seek overrides it immediately. Two-line display shows the current
  line and the next one. `歌词校准 ±` nudges by 250 ms if a particular LRC is
  off, and browser titles are cleaned (`晴天 - 周杰伦 | 网易云音乐`) before the
  lyric lookup.
* **Holds the local music down** while a source is linked, via a 64-byte
  shared-memory handshake with CMS. IMR does not patch or link CMS.
* **Drops everything on exit**: audio sessions restored, lyric windows
  destroyed, handshake cleared. Link state is memory-only, so switching saves
  changes nothing.

## Version 2.2 — removals, fixes, responsiveness

* **Four dead entries are gone for good.** The four UI Automation buttons and
  their input field were removed from the panel, the JS state and command
  handlers, the C++ command dispatch, the source catalogue (their URL fields),
  the build (the whole automation module deleted, its import library no longer
  linked) and the documentation. No accessibility-tree walking, no related
  state, no leftover verbs or handlers remain anywhere in the project.
* **Seek actually seeks.** Dragging the bar used to lose its target because the
  next poll overwrote it with the old SMTC timeline. C++ now anchors a
  monotonic override at the chosen second, updates its cached timeline and
  resumes playback; the front-end holds a pending-seek guard so the thumb
  cannot flash backwards. The override is released as soon as the player
  reports the new position (or after 3 s).
* **All four modes behave.** Optimistic selection + confirmation, tolerant
  inference for players publishing only one switch, invalid-value rejection,
  re-application when a media session appears late, and a strictly guarded
  local fallback for three refused attempts. 顺序播放 never intervenes.
* **Lower latency everywhere.** JS commands wake the 16 ms engine frame through
  a condition variable (sub-millisecond click-to-repaint), snapshots are pushed
  at 10 Hz *and only when the JSON changed*, the front-end rebuilds its lists
  by data signature instead of every frame, irrelevant audio/media sessions are
  skipped before COM/WinRT calls, and every WinRT async operation has a finite
  wait (60 ms metadata / 400 ms commands) so a hung player cannot freeze the
  radio. Final behaviour of every retained feature is unchanged.

## Compatibility with CustomMusicSystem

| Concern | How it is handled |
|---|---|
| Symbols | own `imr::` namespace, own logging macros (`IMR_LOG_*`) |
| DLL / log / INI | `InteractiveMusicRadio.dll`, `InteractiveMusicRadio.log`, `InteractiveMusicRadio.ini` |
| Vanilla music | only *asks* CMS to stand down through the handshake word; never touches CMS's audio |
| Exit | clears the handshake and restores sessions, leaving CMS exactly as it was |

Without CMS the radio still works as a remote; it just cannot duck the game's
music.

## How it works

1. The engine thread polls the audio sessions (`IAudioSessionManager2`, for
   volume and "is this program running") and the OS media sessions (for
   metadata and transport) on separate intervals.
2. Each poll is classified against the catalogue (`src/sources.cpp`). Anything
   that is not one of the six is **ignored**, not listed.
3. Selecting a card writes `state = 1` into the 64-byte block
   `IMR_CMS_HANDSHAKE` — `{magic, version, state, pid, heartbeatMs, flags}` —
   and clears it on release.
4. CMS 2.6.6+ reads that block and, while it is fresh (3 s heartbeat), holds its
   local music layer down, so what you hear is the external source.
5. The lyric window follows the linked track: title changes trigger one online
   look-up, the LRC is parsed, and the current line is scrolled by the same
   interpolated playback position the progress bar uses.

## Front-end

`ui/` holds the PrismaUI front-end — plain HTML / CSS / JavaScript, no build
step:

* `logo.svg` — the teal **IMR** monogram (station logo)
* `icon.svg` — the flat radio station icon
* `index.html` + `style.css` + `radio.js` — the console (desktop panel left,
  web panel right, transport + lyrics below)

**Where it goes:** PrismaUI does *not* resolve `CreateView()` paths against
`Data\` — it prepends `file:///views/` and uses `Data\PrismaUI\views\` as the
base directory ([docs](https://www.prismaui.dev/api/create-view/)). The five
files therefore live in

```
Data\PrismaUI\views\InteractiveMusicRadio\index.html
```

and the path passed to `CreateView` stays `"InteractiveMusicRadio/index.html"`.
Deploying them anywhere else produces a silently blank view; the framework log
says `Failed loading URL ... File URL loading failed`, and IMR logs
`ui: no DOM after 5 s - PrismaUI resolves ...`.

The bridge contract (fixed — these are the names in `src/ui.cpp`):

```
C++ -> JS : window.imrUpdate(jsonString)      // InteropCall("imrUpdate", …)
JS  -> C++: window.imrCommand("verb\nkey=value\n…")
```

The payload travels as a **string**: `InteropCall` passes one string argument, so
a song title containing a quote or a newline can never produce broken JavaScript
source — the JSON is parsed with `JSON.parse` on the JS side.

Commands are newline-separated, and only the first `=` on a line is split:

| verb | keys | meaning |
|---|---|---|
| `select` | `id` | link that source (opens the web player if needed) |
| `clear` | — | unlink, release the local layer |
| `transport` | `act` = play/pause/toggle/next/prev | drive the player via SMTC |
| `volume` | `v` (0..1), `mute` | set the linked source's volume / mute |
| `seek` | `pos` (seconds) | jump to a position and resume from there (SMTC) |
| `playmode` | `mode` = 0/1/2/3 | 随机 / 顺序 / 单曲 / 列表, via SMTC switches |
| `lyrics` | `on` (1/0) | show/hide **our** desktop lyric window |
| `lyricslock` | `on` (1/0) | lock (click-through) / unlock (draggable) it |
| `lyricoffset` | `delta` (ms) | nudge lyric sync |
| `movepanel` | `x`, `y` | save the panel's drag offset |
| `hotkey` | `act` = capture/reset/cancel | record or restore the launcher key |
| `takeover` | `on` (1/0) | master switch for holding the local layer |
| `web` | `id` | open that music site in the system browser |
| `refresh` | — | re-poll sources now |
| `close` / `open` | — | hide / show the view |

## Install

1. **PrismaUI** (required for the visual radio). Copy its
   `SKSE\plugins\PrismaUI.dll` plus the `PrismaUI\` folder (Ultralight runtime,
   `icudt67l.dat`, `cacert.pem`, `libs\*.dll`) into `Data`. As a MO2 mod the
   archive's root is the mod root — enable it in the left pane.
2. **IMR**: copy `SKSE\Plugins\InteractiveMusicRadio.dll` (Chinese build) or
   `Language-EN\InteractiveMusicRadio.dll` (English build, renamed to
   `InteractiveMusicRadio.dll`), the INI next to it, and the
   `PrismaUI\views\InteractiveMusicRadio\` folder to the root of `Data`.
3. Enable both in MO2, launch, and press **F10** (or the key you recorded).

## Build

Same pattern as CMS (VS generator already configured; no vcvars, no proxy env
in the MSBuild subprocess):

```
python build.py            # Release, both language DLLs
```

Output: `build/Release/InteractiveMusicRadio.dll` and
`InteractiveMusicRadio_en.dll`, plus the two test executables (149 pure-logic
checks each: config, catalogue, LRC, play modes and position interpolation).

`extern/PrismaUI_API.h` is copied verbatim from the PrismaUI 1.5.1 release. It
declares the interface; nothing from PrismaUI is compiled or linked here.

> **PrismaUI runtime**: IMR resolves PrismaUI through
> `GetModuleHandle(L"PrismaUI.dll") → GetProcAddress("RequestPluginAPI")`, so the
> plugin loads — and the media-control features keep working — even when
> PrismaUI is absent. Only the visual radio is skipped, and the log says why.

## Notes / limits

* 网页版 controls whatever tab the browser is currently playing. Windows does not
  tell us the site behind a media session, so the three web cards share one
  browser session and only the linked card displays it.
* Lyrics are looked up online (网易云 public lyric API, one request per track,
  `bFetchLyrics=0` to disable). Offline, the window shows the track line instead.
* External audio is not captured or re-streamed inside Skyrim: latency stays at
  zero and no DRM problem appears.
* The handshake is a **heartbeat**: CMS ignores the block once it is more than 3 s
  old, so a crashed IMR releases the local music layer by itself.
* The four modes are applied through SMTC where available. After three refusals
  (`playmode: … refused by the player`) the radio enforces the order itself;
  顺序播放 never intervenes. SMTC has no "pick a random track" call, so
  随机播放 on such a player follows the player's own queue via SkipNext.
* `Show` / `Hide` are PrismaUI renderer calls, so they are queued onto the game
  thread (`SKSE::GetTaskInterface`) instead of being called from the engine
  thread — calling them from the wrong thread silently did nothing, which is why
  the ✕ button appeared dead in 2.0.0.
