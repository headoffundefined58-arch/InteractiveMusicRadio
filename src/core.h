// InteractiveMusicRadio - shared types, config, media-source model.
// No Win32 headers, no RE headers: pure, offline-testable logic.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace imr {

// ---------------------------------------------------------------- logging ---
// Dependency-free logger (plain FILE* + mutex), flushed per line so a crash
// never eats the last lines.  Identical in spirit to CMS but its own symbols,
// so the two plugins share the process without clashing.
// Process-wide steady clock (one shared epoch) so a timestamp taken by the
// SMTC poller can be compared with one taken by the engine loop.
[[nodiscard]] double NowSeconds();

void LogOpen(const std::string& a_path);
void LogClose();
void LogMsg(const char* a_level, const char* a_fmt, ...);
void LogFlush();
void SetVerbose(bool a_on);

#define IMR_LOG(level, ...) imr::LogMsg(level, __VA_ARGS__)
#define IMR_LOG_I(...) IMR_LOG("info", __VA_ARGS__)
#define IMR_LOG_W(...) IMR_LOG("warn", __VA_ARGS__)
#define IMR_LOG_E(...) IMR_LOG("error", __VA_ARGS__)
#define IMR_LOG_D(...) IMR_LOG("debug", __VA_ARGS__)

// ---------------------------------------------------------- audio session ---
// One audio session on the machine.  IMR no longer *lists* every session any
// more (2.0 only serves music apps / music sites); what is left is the volume
// handle: a matched session is how the in-game volume slider reaches the app.
struct AudioSession {
	std::string  id;         // session identifier
	std::string  exe;        // lower-case executable name, e.g. "qqmusic.exe"
	std::string  name;       // display name
	bool         active = false;
	float        volume = 1.0f;
	bool         muted = false;
	bool         captured = false;
	std::uint64_t pid = 0;
};

// ------------------------------------------------------------ media source --
// Two kinds only, as asked for: a local music program, or the web player of the
// same service running in the system browser.
enum class SourceKind : std::uint8_t {
	Desktop = 0,
	Web = 1,
};

// What a source is playing right now.  Filled from the system media transport
// controls (SMTC), so it works for a music app that sits minimised in the tray.
struct TrackInfo {
	std::string title;
	std::string artist;
	std::string album;
	bool        hasMedia = false;
	bool        playing = false;
	double      position = 0.0;     // seconds, corrected to the poll instant
	double      positionAt = 0.0;   // steady seconds when it was sampled
	double      duration = 0.0;     // seconds, 0 = unknown
};

struct MediaSource {
	std::string  id;                    // "qqmusic" | "web-netease" | …
	SourceKind   kind = SourceKind::Desktop;
	std::uint64_t pid = 0;
	bool         running = false;        // process alive / browser has a session
	bool         controlled = false;     // an SMTC session answers our commands
	TrackInfo    track;
	float        volume = 1.0f;
	bool         muted = false;
};

// Why "takeover" is only about our own output: the music app keeps playing in
// its own process; IMR never injects into it and never pipes its audio through
// Skyrim.  When a source is linked, IMR holds the *local* music layer down via
// the CMS handshake, so the external source is what you actually hear.
enum class LinkState : std::uint8_t {
	Disconnected = 0,   // nothing linked
	Listening = 1,      // sources available, none linked
	Linked = 2,         // a source is the live audio
};

// Playback order, the four modes every Chinese player shows in one menu.  The OS
// exposes two separate switches (shuffle on/off, auto-repeat none/track/list);
// this enum is the pair collapsed into the one thing the player actually picks.
enum class PlayMode : std::int8_t {
	Unknown   = -1,
	Shuffle   = 0,   // 随机播放
	Sequential = 1,  // 顺序播放
	RepeatOne = 2,   // 单曲循环
	RepeatAll = 3,   // 列表循环
};

// SMTC -> PlayMode.  a_repeat follows Windows.Media.MediaPlaybackAutoRepeatMode
// (0 none, 1 track, 2 list); a_shuffle is 0/1.  Returns Unknown when the player
// publishes neither switch, which is common and must not be guessed at.
[[nodiscard]] PlayMode PlayModeOf(int a_shuffle, int a_repeat);

// The two switches that produce a mode.  RepeatAll/Shuffle both keep the queue
// going, so shuffling also asks for list repeat.
void PlayModeSwitches(PlayMode a_mode, bool* a_shuffle, int* a_repeat);

// Tolerant variant used for *display*.  A lot of players publish only one of
// the two SMTC switches; the strict PlayModeOf() would call those Unknown and
// the menu would never tick the mode the player is actually in.  Pure so the
// inference is unit-testable.
[[nodiscard]] PlayMode PlayModeOfLoose(int a_shuffle, int a_repeat);

// Interpolate one SMTC timeline sample up to `a_now`.  Shared by the progress
// bar and the lyric clock so both show exactly the same instant.  Pure.
//   a_positionAt  steady-clock stamp of the sample (0 = no interpolation)
//   a_duration    0 / <= 1 means "unknown, do not clamp"
[[nodiscard]] double PositionNow(double a_position, double a_positionAt,
	bool a_playing, double a_now, double a_duration);

struct Snapshot {
	LinkState                 state = LinkState::Disconnected;
	std::string               activeId;      // linked source id
	std::vector<MediaSource>  sources;
	bool                      takeover = false;  // hold local music down
	bool                      lyrics = false;    // our own desktop lyric window
	std::string               lyricNow;          // current lyric line ("" = none)
	double                    capturedGain = 1.0;
	int                       playMode = -1;     // PlayMode of the linked source

	// Layout / hotkey state.  Kept in the snapshot so the front-end is a pure
	// view of the runtime: reload it and the panel comes back where it was.
	int                       panelX = 0;        // px offset from centred
	int                       panelY = 0;
	int                       lyricsKind = 0;    // 0 = desktop app, 1 = web
	bool                      lyricsLocked = true;
	std::string               hotkeyLabel;       // "Ctrl + F10", "未设置", …
	int                       bindState = 0;     // 0 idle, 1 recording
	int                       lyricOffsetMs = 0; // manual lyric sync nudge
	std::uint64_t             seq = 0;
};

// ---------------------------------------------------------------- config ---
struct Config {
	// Master switch.
	bool enabled = true;

	// When linked, duck (rather than fully mute) the linked source while a
	// dialogue is running (0 = mute).
	bool duckInDialogue = true;
	float dialogueDuck = 0.35f;

	// How aggressively we duck the *local* music when a source is linked.
	float localDuck = 1.0f;

	// Polling / refresh.
	int  sessionPollMs = 500;     // audio sessions (volume / running state)
	int  trackPollMs = 1000;      // SMTC metadata
	bool verbose = false;

	// The launcher key.  Since 2.1 it is recorded in-game (press a key while
	// the radio asks for one) and saved here, so no system key is stolen by
	// default and the player picks whatever is free in their load order.
	// GetIDCode() is a DirectInput *scan code*, not a virtual key.
	int  hotkeyKey = 68;           // F10
	int  hotkeyMods = 0;           // 1 Ctrl | 2 Shift | 4 Alt
	int  openKey = 0;              // legacy alias, kept so old ini files load
	bool ctrlInSystemMenu = false; // legacy: ignored since 2.1

	// Panel position, as an offset from its centred default (px).
	int  panelX = 0;
	int  panelY = 0;

	// Our two independent desktop lyric windows: [0] for a desktop player,
	// [1] for a web player.  Each has its own position and lock state; a
	// locked window is click-through again.
	int  lyricX[2] = { -1, -1 };
	int  lyricY[2] = { -1, -1 };
	bool lyricLocked[2] = { true, true };

	bool  lyricsOnStart = false;
	bool  fetchLyrics = true;      // look lyrics up online (best effort)
	int   lyricFontSize = 30;
	int   lyricOffsetMs = 0;       // manual sync nudge, milliseconds

	std::string iniPath;
	std::string pluginDir;   // SKSE\Plugins
	std::string dataDir;     // <game>\Data

	bool LoadFromFile(const std::string& a_path);
	bool SaveToFile(const std::string& a_path) const;
	void ApplyKey(std::string_view a_key, std::string_view a_value);
};

// -------------------------------------------------------- offline helpers ---
[[nodiscard]] LinkState NextLinkState(LinkState a_cur, bool a_haveSources,
    bool a_selectedActive);

// DirectInput scan code -> a short, human-readable key name.  Pure so the
// hotkey recorder can be unit-tested without a game.
[[nodiscard]] std::string KeyName(int a_scanCode);

// "Ctrl + Shift + F10", "F10", "未设置".  a_mods is 1 Ctrl | 2 Shift | 4 Alt.
[[nodiscard]] std::string HotkeyLabel(int a_key, int a_mods);

}  // namespace imr
