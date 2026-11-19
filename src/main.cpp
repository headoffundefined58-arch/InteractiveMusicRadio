// InteractiveMusicRadio - SKSE plugin entry, engine loop, lifecycle.
//
// Lifecycle, stated once so it cannot drift:
//   * loads alongside CustomMusicSystem.dll with zero symbol clashes (own
//     namespace, own log, own ini, own dll name);
//   * drives music *programs* and music *sites* through the OS media session
//     (SMTC) - never by injecting into another process;
//   * two independent desktop lyric windows (desktop app / web player) are the
//     only lyric overlays it touches;
//   * on game exit, every adjusted audio session is restored, both lyric
//     windows destroyed and the handshake block cleared;
//   * link state lives in memory only, so loading another save changes nothing.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <SKSE/Logger.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core.h"
#include "handshake.h"
#include "lrc.h"
#include "lyrics.h"
#include "nowplaying.h"
#include "platform.h"
#include "session.h"
#include "sources.h"
#include "ui.h"

namespace fs = std::filesystem;

namespace {

using namespace imr;

Config                 g_cfg;
AudioSessionController g_sessions;
CmsHandshake           g_handshake;
NowPlaying             g_media;
LyricsOverlay          g_lyrics[2];   // [0] desktop player, [1] web player

// The launcher binding is written on the engine thread (hotkey recorder /
// reset) but read on the game thread (HotkeySink::ProcessEvent, InstallHotkey).
// g_cfg.hotkeyKey/hotkeyMods stay as the *persisted* values (Load/Save), while
// these atomics are the lock-free runtime truth for every key comparison.
std::atomic<int> g_hotkeyKey{ 68 };    // F10 scan code
std::atomic<int> g_hotkeyMods{ 0 };    // 1 Ctrl | 2 Shift | 4 Alt
Snapshot               g_snap;

std::thread       g_engine;
std::atomic<bool> g_run{ false };
std::atomic<bool> g_started{ false };

// Last poll results, kept so commands can resolve a source to its SMTC app id
// and to the process that owns its audio session.
std::vector<AudioSession>                       g_audioCache;
std::vector<SessionInfo>                        g_mediaCache;
std::unordered_map<std::string, std::string>    g_aumidOf;   // source id -> SMTC aumid
std::unordered_map<std::string, std::uint64_t>  g_pidOf;     // source id -> process
std::unordered_map<std::string, int>            g_modeOf;    // source id -> reported PlayMode

// One lyric look-up deck per overlay: a web player must never inherit the
// desktop app's lyric, and vice versa.
struct LyricDeck {
	std::mutex   mtx;
	std::string  lrc;
	std::string  trackKey;
	bool         ready = false;
};
LyricDeck g_decks[2];

// ------------------------------------------------------------- seek clock ---
// The player may need up to a second to report a new timeline after a seek.
// Until it does (or 3 s elapse), the bar and the lyrics run from the point the
// user chose instead of snapping back to the stale SMTC value.
bool   g_seekActive = false;
double g_seekPos = 0.0;
double g_seekAt = 0.0;

// ------------------------------------------------------------ play modes ---
// What the user picked, shown immediately.  A poll must never overwrite a
// fresh selection: many players publish neither SMTC switch, which used to
// make the menu drop its tick on the very next frame.
struct ModeOverride {
	bool        active = false;
	std::string sourceId;
	PlayMode    mode = PlayMode::Unknown;
};
ModeOverride g_modeOverride;

// A mode chosen before the media session existed, or refused by the player,
// is re-applied a few times before IMR falls back to driving the order itself.
struct PendingMode {
	bool        active = false;
	std::string sourceId;
	PlayMode    mode = PlayMode::Unknown;
	double      nextAt = 0.0;
	int         attempts = 0;
};
PendingMode g_pendingMode;

// Only enabled when the player refused the SMTC switches outright.  Strictly
// guarded so it can never fire on a normal player or twice for one track.
struct LocalMode {
	bool        active = false;
	std::string sourceId;
	PlayMode    mode = PlayMode::Unknown;
	std::string trackKey;
	double      lastAction = -100.0;
};
LocalMode g_localMode;

// ------------------------------------------------------------- wake signal ---
// JS commands used to wait for the next 100 ms engine tick.  Waking the loop
// on arrival takes the command-to-repaint latency down to sub-millisecond.
std::mutex              g_wakeMtx;
std::condition_variable g_wakeCv;
bool                    g_wakeFlag = false;

// Hotkey recorder / launcher.  Both the SKSE input sink and a Win32 async-key
// poller can trigger a toggle (PrismaUI may hold keyboard focus); a 300 ms
// dedup makes exactly one of them win.
std::atomic<bool>   g_capturing{ false };
std::atomic<bool>   g_menuBlocked{ false };
std::atomic<double> g_captureAfter{ 0.0 };
std::atomic<double> g_lastToggle{ -100.0 };
bool                g_hotPrev = false;   // poller only
bool                g_escPrev = false;
std::atomic<bool>   g_wantAutoLink{ false };

double Now() { return NowSeconds(); }

void WakeEngine()
{
	{
		std::lock_guard<std::mutex> lk(g_wakeMtx);
		g_wakeFlag = true;
	}
	g_wakeCv.notify_one();
}

void SaveConfig()
{
	if (g_cfg.iniPath.empty()) return;
	if (!g_cfg.SaveToFile(g_cfg.iniPath)) {
		IMR_LOG_W("config: could not write %s", g_cfg.iniPath.c_str());
	}
}

// ------------------------------------------------------------ source table ---

const SessionInfo* MediaFor(const std::string& a_sourceId)
{
	for (const auto& m : g_mediaCache) {
		if (const SourceDef* d = ClassifyApp(m.aumid, m.aumid, m.title)) {
			if (a_sourceId == d->id) return &m;
		}
	}
	return nullptr;
}

const AudioSession* AudioFor(const std::string& a_sourceId)
{
	for (const auto& s : g_audioCache) {
		if (const SourceDef* d = ClassifyApp(s.exe, "", "")) {
			if (a_sourceId == d->id) return &s;
		}
	}
	return nullptr;
}

const SessionInfo* BrowserMedia()
{
	const SessionInfo* idle = nullptr;
	for (const auto& m : g_mediaCache) {
		if (!IsBrowserExe(m.aumid)) continue;
		if (m.playing) return &m;
		if (!idle) idle = &m;
	}
	return idle;
}

int ActiveKind()
{
	const SourceDef* d = FindSource(g_snap.activeId);
	return d && d->kind == SourceKind::Web ? 1 : 0;
}

const MediaSource* ActiveSource()
{
	for (const auto& s : g_snap.sources) {
		if (s.id == g_snap.activeId) return &s;
	}
	return nullptr;
}

// The one authoritative "where is playback right now" value.  Used for the
// progress bar AND the lyric highlight, so they can never disagree.
double EffectivePosition(const TrackInfo& a_track, double a_now)
{
	const double smtc = PositionNow(a_track.position, a_track.positionAt,
		a_track.playing, a_now, a_track.duration);

	if (g_seekActive) {
		const double age = a_now - g_seekAt;
		const double guess = g_seekPos + std::max(0.0, age);
		// SMTC caught up with the jump -> hand control back to the player.
		if (age > 3.0 || std::fabs(smtc - guess) < 1.5) {
			g_seekActive = false;
		} else {
			return a_track.duration > 1.0
				? std::min(guess, a_track.duration) : guess;
		}
	}
	return smtc;
}

// Rebuilds the six-entry list from the two caches.  The UI never sees anything
// outside this catalogue - that is the "no more listening to every source"
// requirement, enforced in one place.
void RefreshSources()
{
	g_aumidOf.clear();
	g_pidOf.clear();
	g_modeOf.clear();

	const double now = NowSeconds();
	const SessionInfo* browser = BrowserMedia();
	std::vector<MediaSource> list;
	list.reserve(Catalog().size());

	for (const auto& def : Catalog()) {
		MediaSource ms;
		ms.id = def.id;
		ms.kind = def.kind;

		if (def.kind == SourceKind::Desktop) {
			if (const auto* a = AudioFor(def.id)) {
				ms.running = true;
				ms.pid = a->pid;
				ms.volume = a->volume;
				ms.muted = a->muted;
				g_pidOf[def.id] = a->pid;
			}
			if (const auto* m = MediaFor(def.id)) {
				ms.running = true;
				ms.controlled = true;
				ms.track.hasMedia = !m->title.empty();
				NormalizeTrack(m->title, m->artist, &ms.track.title, &ms.track.artist);
				ms.track.album = m->album;
				ms.track.playing = m->playing;
				ms.track.position = m->position;
				ms.track.positionAt = m->positionAt;
				ms.track.duration = m->duration;
				g_aumidOf[def.id] = m->aumid;
				g_modeOf[def.id] =
					static_cast<int>(PlayModeOfLoose(m->shuffle, m->repeat));
			}
		} else {
			// All three web entries share the browser's media session; only the
			// linked one shows its metadata, otherwise three cards would claim
			// to play the same song.
			ms.running = browser != nullptr;
			if (browser && g_snap.activeId == def.id) {
				ms.controlled = true;
				ms.track.hasMedia = !browser->title.empty();
				NormalizeTrack(browser->title, browser->artist,
					&ms.track.title, &ms.track.artist);
				ms.track.album = browser->album;
				ms.track.playing = browser->playing;
				ms.track.position = browser->position;
				ms.track.positionAt = browser->positionAt;
				ms.track.duration = browser->duration;
				g_aumidOf[def.id] = browser->aumid;
				g_modeOf[def.id] =
					static_cast<int>(PlayModeOfLoose(browser->shuffle, browser->repeat));
			}
		}

		// Publish an already-interpolated position.  The front-end is a pure
		// view and must not have to know about the shared steady clock.
		if (ms.id == g_snap.activeId) {
			ms.track.position = EffectivePosition(ms.track, now);
		} else {
			ms.track.position = PositionNow(ms.track.position, ms.track.positionAt,
				ms.track.playing, now, ms.track.duration);
		}
		ms.track.positionAt = now;

		list.push_back(std::move(ms));
	}

	g_snap.sources = std::move(list);

	bool anyRunning = false;
	bool activePlaying = false;
	for (const auto& s : g_snap.sources) {
		if (s.running) anyRunning = true;
		if (s.id == g_snap.activeId && s.track.playing) activePlaying = true;
	}
	g_snap.state = NextLinkState(g_snap.state, anyRunning, activePlaying);
	g_snap.takeover = g_cfg.enabled && !g_snap.activeId.empty() && activePlaying;

	// The mode belongs to the linked source, not to the panel.  A fresh user
	// selection wins until the player actually reports that same mode back.
	int pm = -1;
	const auto modeIt = g_modeOf.find(g_snap.activeId);
	if (modeIt != g_modeOf.end()) pm = modeIt->second;
	if (g_modeOverride.active && g_modeOverride.sourceId == g_snap.activeId) {
		if (pm == static_cast<int>(g_modeOverride.mode)) {
			g_modeOverride.active = false;    // confirmed by the player
		} else {
			pm = static_cast<int>(g_modeOverride.mode);
		}
	}
	g_snap.playMode = pm;
	g_snap.panelX = g_cfg.panelX;
	g_snap.panelY = g_cfg.panelY;
	// hotkeyLabel is updated only when the binding changes (ApplyCommand /
	// StartAll), not every 16 ms frame - it used to rebuild a string 60x/s.
	g_snap.lyricOffsetMs = g_cfg.lyricOffsetMs;
	const int kind = ActiveKind();
	g_snap.lyricsKind = kind;
	g_snap.lyricsLocked = g_cfg.lyricLocked[kind];
}

// ------------------------------------------------------------- play modes ---

void TickPlayMode(double a_now)
{
	if (!g_pendingMode.active) return;
	if (g_pendingMode.sourceId != g_snap.activeId) {
		g_pendingMode.active = false;   // the user switched sources
		g_localMode.active = false;
		return;
	}
	if (a_now < g_pendingMode.nextAt) return;

	const auto it = g_aumidOf.find(g_snap.activeId);
	if (it == g_aumidOf.end()) {
		// No session yet (a web source may still be opening).  Do not count
		// this as a refusal - just keep waiting.
		g_pendingMode.nextAt = a_now + 0.5;
		return;
	}
	if (g_media.SetPlayMode(it->second, g_pendingMode.mode)) {
		IMR_LOG_I("playmode: applied on retry %d", g_pendingMode.attempts + 1);
		g_pendingMode.active = false;
		return;
	}

	++g_pendingMode.attempts;
	if (g_pendingMode.attempts >= 3) {
		g_pendingMode.active = false;
		g_localMode = { true, g_pendingMode.sourceId, g_pendingMode.mode,
			std::string(), -100.0 };
		IMR_LOG_W("playmode: player refused the SMTC switches; mode %d is now "
			"enforced by the radio at the end of each track",
			static_cast<int>(g_pendingMode.mode));
	} else {
		g_pendingMode.nextAt = a_now + 0.8;
	}
}

// Last-resort playback order for players that ignore the SMTC switches.  Only
// ever reached after three refusals, and guarded so a single track can trigger
// at most one action - a normal player is never touched.
void EnforceLocalMode(double a_now)
{
	if (!g_localMode.active || g_localMode.sourceId != g_snap.activeId) return;
	if (g_localMode.mode != PlayMode::RepeatOne &&
		g_localMode.mode != PlayMode::RepeatAll &&
		g_localMode.mode != PlayMode::Shuffle) return;   // 顺序播放: 不干预

	const MediaSource* s = ActiveSource();
	if (!s || !s->track.playing || s->track.duration < 10.0) return;
	if (s->track.position < s->track.duration - 0.4) return;
	if (a_now - g_localMode.lastAction < 3.0) return;

	const auto it = g_aumidOf.find(g_snap.activeId);
	if (it == g_aumidOf.end()) return;

	const bool repeatOne = g_localMode.mode == PlayMode::RepeatOne;
	if (!repeatOne) {
		// next / shuffle change the track, so keying on the track identity
		// prevents a single end-of-track from firing twice.
		const std::string key = s->track.title + "|" + s->track.artist + "|" +
			s->track.album;
		if (key == g_localMode.trackKey) return;
		g_localMode.trackKey = key;
	}
	// RepeatOne replays the *same* track in place: its key never changes, so
	// the identity check above must be skipped or the loop dies after the
	// first restart.  Re-arming is instead implicit - after Seek(0) the
	// position drops below duration-0.4 and must climb back to the end.
	g_localMode.lastAction = a_now;
	if (repeatOne) {
		if (g_media.Seek(it->second, 0.0)) {
			g_seekPos = 0.0;
			g_seekAt = a_now;
			g_seekActive = true;
		}
		IMR_LOG_I("playmode: 单曲循环 - restarted the track locally");
	} else {
		// SMTC has no "pick a random track" call; SkipNext follows the
		// player's own queue (and its shuffle, if the player has one).
		g_media.Command(it->second, "next");
		IMR_LOG_I("playmode: %s - advanced the queue locally",
			g_localMode.mode == PlayMode::Shuffle ? "随机播放" : "列表循环");
	}
}

// --------------------------------------------------------------- lyrics ------

void RequestLyrics(int a_kind, const std::string& a_trackKey,
	const std::string& a_title, const std::string& a_artist)
{
	if (!g_cfg.fetchLyrics || a_title.empty()) return;
	std::string song;
	std::string singer;
	NormalizeTrack(a_title, a_artist, &song, &singer);
	const std::string query = LyricsQuery(song, singer);
	if (query.empty()) return;

	// Dedup lives solely in RefreshLyrics' trackKey check.  The old code also
	// deduped here on the album-less query, so a song reappearing on a different
	// album (new trackKey, same query) was silently skipped and its lyric never
	// loaded.  Every call reaching here is a genuinely new track: clear + fetch.
	LyricDeck& deck = g_decks[a_kind];
	{
		std::lock_guard<std::mutex> lk(deck.mtx);
		deck.lrc.clear();
		deck.ready = false;
	}

	// Detached on purpose: a look-up can take seconds and the engine loop has
	// a fast lyric tick to keep on schedule.  Only a still-current trackKey may
	// publish its result, so a slow old look-up cannot overwrite a newer lyric.
	std::thread([a_kind, a_trackKey, song, singer] {
		std::string lrc;
		FetchLyrics(song, singer, &lrc);
		LyricDeck& d = g_decks[a_kind];
		std::lock_guard<std::mutex> lk(d.mtx);
		if (d.trackKey != a_trackKey) return;   // a newer track won
		d.lrc = std::move(lrc);
		d.ready = true;
	}).detach();
}

bool TakeLyric(int a_kind, std::string* a_lrc)
{
	LyricDeck& d = g_decks[a_kind];
	std::lock_guard<std::mutex> lk(d.mtx);
	if (!d.ready) return false;
	d.ready = false;
	if (a_lrc) *a_lrc = d.lrc;
	return true;
}

void RefreshLyrics()
{
	const MediaSource* linked = ActiveSource();
	const int kind = linked && linked->kind == SourceKind::Web ? 1 : 0;

	for (int k = 0; k < 2; ++k) {
		// Only the overlay belonging to the linked kind is on screen; the other
		// keeps its own lyric, position and lock state for when you switch back.
		g_lyrics[k].SetVisible(g_snap.lyrics && k == kind &&
			linked && linked->track.hasMedia);
	}
	if (!linked || !linked->track.hasMedia) {
		g_snap.lyricNow.clear();
		for (auto& l : g_lyrics) l.Pump();
		return;
	}

	const std::string trackKey = linked->track.title + "|" + linked->track.artist +
		"|" + linked->track.album;
	LyricDeck& deck = g_decks[kind];
	if (trackKey != deck.trackKey) {
		deck.trackKey = trackKey;
		g_lyrics[kind].SetTrack(linked->track.title, linked->track.artist,
			linked->track.album);
		RequestLyrics(kind, trackKey, linked->track.title, linked->track.artist);
	}
	std::string lrc;
	if (TakeLyric(kind, &lrc)) {
		g_lyrics[kind].SetLyric(ParseLrc(lrc));
		IMR_LOG_I("lyrics[%d]: '%s - %s' -> %zu bytes", kind,
			linked->track.title.c_str(), linked->track.artist.c_str(), lrc.size());
	}

	// RefreshSources() already produced the interpolated "now" position; the
	// only extra term is the user's lyric calibration.
	const double pos = linked->track.position +
		static_cast<double>(g_cfg.lyricOffsetMs) / 1000.0;
	g_lyrics[kind].Tick(pos);
	g_snap.lyricNow = g_lyrics[kind].CurrentLine();
	// Both windows must be painted: one is hidden, but each still owns its
	// own layered bitmap and message queue.
	for (auto& l : g_lyrics) l.Pump();
}

// ------------------------------------------------------------- commands ------

void OpenUrl(const std::string& a_url)
{
	if (a_url.empty()) return;
	ShellExecuteA(nullptr, "open", a_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	IMR_LOG_I("web: opened %s", a_url.c_str());
}

void ApplyCommand(const ui::Command& a_cmd)
{
	const std::string& verb = a_cmd.verb;

	if (verb == "select") {
		const std::string id = a_cmd.at("id");
		const SourceDef* def = FindSource(id);
		if (!def) return;

		if (g_snap.activeId != id) {
			g_modeOverride.active = false;
			g_pendingMode.active = false;
			g_localMode.active = false;
			g_seekActive = false;
		}
		g_snap.activeId = id;
		g_snap.takeover = g_cfg.enabled;
		g_snap.state = LinkState::Linked;
		IMR_LOG_I("ui: linked '%s' (local music held down)", id.c_str());
		if (def->kind == SourceKind::Web && !BrowserMedia()) OpenUrl(def->url);

	} else if (verb == "clear") {
		g_snap.activeId.clear();
		g_snap.takeover = false;
		g_snap.state = LinkState::Listening;
		g_modeOverride.active = false;
		g_pendingMode.active = false;
		g_localMode.active = false;
		g_seekActive = false;   // stale anchor must not leak into the next link
		IMR_LOG_I("ui: link cleared (local music released)");

	} else if (verb == "transport") {
		const std::string act = a_cmd.at("act");
		const auto it = g_aumidOf.find(g_snap.activeId);
		if (it != g_aumidOf.end() && g_media.Command(it->second, act.c_str())) {
			IMR_LOG_I("transport: %s -> ok", act.c_str());
		} else {
			IMR_LOG_W("transport: %s failed (no media session for '%s')", act.c_str(),
				g_snap.activeId.c_str());
		}

	} else if (verb == "seek") {
		double pos = -1.0;
		try { pos = std::stod(a_cmd.at("pos")); } catch (...) { return; }
		if (pos < 0.0) return;
		const auto it = g_aumidOf.find(g_snap.activeId);
		if (it == g_aumidOf.end()) {
			IMR_LOG_W("seek: no media session for '%s'", g_snap.activeId.c_str());
			return;
		}

		MediaSource* linked = nullptr;
		for (auto& s : g_snap.sources) {
			if (s.id == g_snap.activeId) { linked = &s; break; }
		}
		if (linked && linked->track.duration > 1.0) {
			pos = std::min(pos, linked->track.duration);
		}
		if (!g_media.Seek(it->second, pos)) {
			IMR_LOG_W("seek: %.2fs refused by the player", pos);
			return;
		}

		// Anchor the immediate push and the short override window at the chosen
		// point.  Only the *view* (g_snap.sources) is seeded here.  Never seed
		// g_mediaCache: EffectivePosition() compares the SMTC-derived position
		// against our guess, and seeding the cache makes both sides identical on
		// the very next frame - so the 3 s guard cancelled itself and the bar
		// snapped back as soon as the player reported its stale timeline.
		g_seekPos = pos;
		g_seekAt = NowSeconds();
		g_seekActive = true;
		if (linked) {
			linked->track.position = pos;
			linked->track.positionAt = g_seekAt;
		}
		// "Continue from there": a paused player must resume at the new point.
		if (!linked || !linked->track.playing) g_media.Command(it->second, "play");
		IMR_LOG_I("seek: %.2fs -> ok, playing from here", pos);

	} else if (verb == "volume") {
		const auto it = g_pidOf.find(g_snap.activeId);
		if (it == g_pidOf.end()) return;
		float v = 1.0f;
		try { v = std::stof(a_cmd.at("v")); } catch (...) { return; }
		v = std::clamp(v, 0.0f, 1.0f);
		g_sessions.SetVolumeForPid(it->second, v, a_cmd.flag("mute"));
		for (auto& s : g_snap.sources) {
			if (s.id == g_snap.activeId) s.volume = v;
		}
		g_snap.capturedGain = v;

	} else if (verb == "takeover") {
		g_snap.takeover = a_cmd.flag("on") && g_cfg.enabled;
		IMR_LOG_I("ui: takeover %s", g_snap.takeover ? "ON" : "OFF");

	} else if (verb == "playmode") {
		int mode = -1;
		try { mode = std::stoi(a_cmd.at("mode")); } catch (...) { return; }
		if (mode < 0 || mode > 3) {
			IMR_LOG_W("playmode: rejected invalid mode %d", mode);
			return;
		}
		const PlayMode pm = static_cast<PlayMode>(mode);
		g_modeOverride = { true, g_snap.activeId, pm };
		g_snap.playMode = mode;
		g_localMode.active = false;

		const auto it = g_aumidOf.find(g_snap.activeId);
		bool applied = false;
		if (it != g_aumidOf.end()) {
			applied = g_media.SetPlayMode(it->second, pm);
		} else {
			IMR_LOG_I("playmode: waiting for a media session for '%s'",
				g_snap.activeId.c_str());
		}
		// Re-apply when the session appears, or when the player refused it.
		g_pendingMode = { !applied, g_snap.activeId, pm, NowSeconds() + 0.6, 0 };
		IMR_LOG_I("playmode: %d %s", mode, applied ? "applied" : "queued for retry");

	} else if (verb == "web") {
		if (const SourceDef* def = FindSource(a_cmd.at("id"))) OpenUrl(def->url);

	} else if (verb == "lyrics") {
		g_snap.lyrics = a_cmd.flag("on");
		IMR_LOG_I("ui: desktop lyrics %s", g_snap.lyrics ? "ON" : "OFF");

	} else if (verb == "lyricslock") {
		const int kind = ActiveKind();
		const bool locked = a_cmd.flag("on");
		g_cfg.lyricLocked[kind] = locked;
		g_lyrics[kind].SetLocked(locked);
		g_snap.lyricsLocked = locked;
		SaveConfig();

	} else if (verb == "lyricoffset") {
		int delta = 0;
		try { delta = std::stoi(a_cmd.at("delta")); } catch (...) { return; }
		g_cfg.lyricOffsetMs = std::clamp(g_cfg.lyricOffsetMs + delta, -10000, 10000);
		g_snap.lyricOffsetMs = g_cfg.lyricOffsetMs;
		SaveConfig();
		IMR_LOG_I("lyrics: offset now %d ms", g_cfg.lyricOffsetMs);

	} else if (verb == "movepanel") {
		int x = 0, y = 0;
		try {
			x = std::stoi(a_cmd.at("x"));
			y = std::stoi(a_cmd.at("y"));
		} catch (...) { return; }
		g_cfg.panelX = std::clamp(x, -3000, 3000);
		g_cfg.panelY = std::clamp(y, -3000, 3000);
		g_snap.panelX = g_cfg.panelX;
		g_snap.panelY = g_cfg.panelY;
		SaveConfig();

	} else if (verb == "hotkey") {
		const std::string act = a_cmd.at("act");
		if (act == "capture") {
			g_capturing.store(true, std::memory_order_release);
			g_captureAfter.store(NowSeconds() + 0.35, std::memory_order_release);
			g_snap.bindState = 1;
			IMR_LOG_I("hotkey: recording - press the new key (Esc cancels)");
		} else if (act == "reset") {
			g_cfg.hotkeyKey = 68;
			g_cfg.hotkeyMods = 0;
			g_hotkeyKey.store(68, std::memory_order_release);
			g_hotkeyMods.store(0, std::memory_order_release);
			g_capturing.store(false, std::memory_order_release);
			g_snap.bindState = 0;
			g_snap.hotkeyLabel = HotkeyLabel(68, 0);
			SaveConfig();
			IMR_LOG_I("hotkey: reset to F10");
		} else if (act == "cancel") {
			g_capturing.store(false, std::memory_order_release);
			g_snap.bindState = 0;
		}

	} else if (verb == "refresh") {
		IMR_LOG_D("ui: refresh requested");

	} else if (verb == "close") {
		ui::Show(false);

	} else if (verb == "open") {
		ui::Show(true);

	} else {
		IMR_LOG_W("ui: unknown command '%s'", verb.c_str());
	}
}

// ---------------------------------------------------------------- engine -----

void TryToggle()
{
	const double now = NowSeconds();
	double prev = g_lastToggle.load(std::memory_order_acquire);
	if (now - prev < 0.30) return;
	if (!g_lastToggle.compare_exchange_strong(prev, now)) return;
	g_wantAutoLink.store(true, std::memory_order_release);
	ui::Toggle();
}

// "优先连接本地音乐程序": opening the panel with nothing linked picks a running
// desktop player, only if it is actually playing.
void AutoLinkDesktop()
{
	if (!g_snap.activeId.empty()) return;
	for (const auto& s : g_snap.sources) {
		if (s.kind != SourceKind::Desktop || !s.controlled || !s.track.playing) continue;
		g_snap.activeId = s.id;
		g_snap.state = LinkState::Linked;
		IMR_LOG_I("ui: auto-linked desktop source '%s'", s.id.c_str());
		ui::PushNow();
		return;
	}
}

int CurrentMods()
{
	int m = 0;
	if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= 1;
	if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= 2;
	if (GetAsyncKeyState(VK_MENU) & 0x8000) m |= 4;
	return m;
}

// Scan code -> virtual key is a constant table.  Building it once keeps the
// 60 Hz hotkey poll (and the recorder's 255-key sweep) free of repeated
// MapVirtualKeyW calls.
const std::array<UINT, 256> kVkOfScan = [] {
	std::array<UINT, 256> a{};
	for (int sc = 1; sc <= 0xFF; ++sc) {
		a[static_cast<std::size_t>(sc)] =
			MapVirtualKeyW(static_cast<UINT>(sc), MAPVK_VSC_TO_VK);
	}
	return a;
}();

// Polling the physical keyboard is what makes the hotkey work even while the
// PrismaUI view holds keyboard focus.  The SKSE sink below is a second path;
// TryToggle() dedups the two.
void PollLauncher()
{
	const double now = NowSeconds();

	if (g_capturing.load(std::memory_order_acquire)) {
		if (now < g_captureAfter.load(std::memory_order_acquire)) return;
		for (int sc = 1; sc <= 0xFF; ++sc) {
			const UINT vk = kVkOfScan[static_cast<std::size_t>(sc)];
			if (!vk || vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
				vk == VK_LWIN || vk == VK_RWIN) continue;
			if ((GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) == 0) continue;

			if (sc == 1) {   // Esc = cancel
				IMR_LOG_I("hotkey: recording cancelled");
			} else {
				g_cfg.hotkeyKey = sc;
				g_cfg.hotkeyMods = CurrentMods();
				g_hotkeyKey.store(sc, std::memory_order_release);
				g_hotkeyMods.store(g_cfg.hotkeyMods, std::memory_order_release);
				SaveConfig();
				g_snap.hotkeyLabel = HotkeyLabel(sc, g_cfg.hotkeyMods);
				IMR_LOG_I("hotkey: bound to %s", g_snap.hotkeyLabel.c_str());
			}
			g_capturing.store(false, std::memory_order_release);
			g_snap.bindState = 0;
			ui::PushNow();
			return;
		}
		return;
	}

	const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
	if (esc && !g_escPrev && ui::Shown()) ui::Show(false);
	g_escPrev = esc;

	const int hkKey = g_hotkeyKey.load(std::memory_order_acquire);
	const int hkMods = g_hotkeyMods.load(std::memory_order_acquire);
	if (g_menuBlocked.load(std::memory_order_acquire) || hkKey <= 0) {
		g_hotPrev = false;
		return;
	}
	const UINT vk = (hkKey > 0 && hkKey < 256)
		? kVkOfScan[static_cast<std::size_t>(hkKey)] : 0;
	const bool down = vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
	const bool match = down && CurrentMods() == hkMods;
	if (match && !g_hotPrev) TryToggle();
	g_hotPrev = match;
}

void EngineLoop()
{
	// Woken by a JS command, otherwise a light 16 ms frame.  Lyrics stay on
	// the beat, commands no longer wait for the next 100 ms tick, and the
	// progress bar is repainted at 10 Hz without rebuilding the whole page.
	constexpr auto kFrame = std::chrono::milliseconds(16);

	double lastPoll = 0.0;
	double lastTrackPoll = 0.0;
	double lastSeq = 0.0;

	// Everything COM-shaped is initialised on this thread, not on the game
	// thread: the apartment must belong to the thread that will call into it.
	std::string err;
	if (g_media.Init(&err)) {
		IMR_LOG_I("OS media session (SMTC) ready");
	} else {
		IMR_LOG_W("media session unavailable: %s - transport control is off", err.c_str());
	}
	if (g_sessions.Init(&err)) {
		IMR_LOG_I("audio session controller ready");
	} else {
		IMR_LOG_W("audio session controller unavailable: %s", err.c_str());
	}

	// Two independent lyric windows: dragging either one saves its own slot.
	for (int k = 0; k < 2; ++k) {
		if (!g_lyrics[k].Create(g_cfg, k)) {
			IMR_LOG_W("lyrics[%d]: window unavailable", k);
			continue;
		}
		g_lyrics[k].SetMoveCallback([k](int a_x, int a_y) {
			g_cfg.lyricX[k] = a_x;
			g_cfg.lyricY[k] = a_y;
			SaveConfig();
			IMR_LOG_I("lyrics[%d]: moved to %d,%d", k, a_x, a_y);
		});
	}

	while (g_run.load(std::memory_order_acquire)) {
		const double now = Now();

		if (now - lastPoll >= static_cast<double>(g_cfg.sessionPollMs) / 1000.0) {
			lastPoll = now;
			g_sessions.Poll(g_audioCache);
		}
		if (now - lastTrackPoll >= static_cast<double>(g_cfg.trackPollMs) / 1000.0) {
			lastTrackPoll = now;
			g_media.Poll(g_mediaCache);
		}

		if (g_wantAutoLink.exchange(false, std::memory_order_acq_rel)) AutoLinkDesktop();

		RefreshSources();
		TickPlayMode(now);
		EnforceLocalMode(now);
		RefreshLyrics();
		PollLauncher();

		if (now - lastSeq >= 0.10) {
			lastSeq = now;
			++g_snap.seq;
		}

		g_handshake.SetTakeover(g_snap.takeover);
		ui::Tick(now, 0.10);

		{
			std::unique_lock<std::mutex> lk(g_wakeMtx);
			g_wakeCv.wait_for(lk, kFrame, [] { return g_wakeFlag; });
			g_wakeFlag = false;
		}
	}
}

// --------------------------------------------------------------- hotkey ------

constexpr std::uint32_t kScanEscape = 0x01;

class HotkeySink final : public RE::BSTEventSink<RE::InputEvent*> {
public:
	RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
		RE::BSTEventSource<RE::InputEvent*>*) override
	{
		if (!a_events) return RE::BSEventNotifyControl::kContinue;
		auto* ui = RE::UI::GetSingleton();
		g_menuBlocked.store(ui && (ui->IsMenuOpen("Console") ||
			ui->IsMenuOpen("Book Menu") || ui->IsMenuOpen("RaceSex Menu")),
			std::memory_order_release);

		// While recording, only the Win32 poller handles keys - PrismaUI may own
		// keyboard focus and the sink would miss them.
		if (g_capturing.load(std::memory_order_acquire)) {
			return RE::BSEventNotifyControl::kContinue;
		}

		for (auto* e = *a_events; e; e = e->next) {
			if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
			auto* btn = e->AsButtonEvent();
			if (!btn || !btn->IsDown()) continue;
			if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;

			const auto key = static_cast<std::uint32_t>(btn->GetIDCode());
			if (key == kScanEscape) {
				if (ui::Shown()) ui::Show(false);       // hard exit, always
				continue;
			}
			if (g_menuBlocked.load(std::memory_order_acquire)) continue;
			const int hkKey = g_hotkeyKey.load(std::memory_order_acquire);
			if (hkKey > 0 && key == static_cast<std::uint32_t>(hkKey) &&
				CurrentMods() == g_hotkeyMods.load(std::memory_order_acquire)) {
				TryToggle();
			}
		}
		return RE::BSEventNotifyControl::kContinue;
	}
};

HotkeySink g_hotkey;
bool       g_hotkeyInstalled = false;

void InstallHotkey()
{
	if (!g_hotkeyInstalled) {
		if (auto* mgr = RE::BSInputDeviceManager::GetSingleton()) {
			mgr->AddEventSink(&g_hotkey);
			g_hotkeyInstalled = true;
			IMR_LOG_I("hotkey: %s toggles the radio (change it in the panel)",
				HotkeyLabel(g_hotkeyKey.load(std::memory_order_acquire),
					g_hotkeyMods.load(std::memory_order_acquire)).c_str());
		}
	}
}

void RemoveHotkey()
{
	if (!g_hotkeyInstalled) return;
	if (auto* mgr = RE::BSInputDeviceManager::GetSingleton()) {
		mgr->RemoveEventSink(&g_hotkey);
	}
	g_hotkeyInstalled = false;
}

// ------------------------------------------------------------- lifecycle -----

void StartAll()
{
	if (g_started.exchange(true)) return;

	g_cfg.dataDir = platform::DataDir();
	g_cfg.pluginDir = platform::PluginDir();

	fs::path logDir;
	if (auto skseLog = SKSE::log::log_directory()) logDir = *skseLog;
	if (logDir.empty()) logDir = fs::current_path();
	const std::string logPath = (logDir / "InteractiveMusicRadio.log").string();
	LogOpen(logPath);

	IMR_LOG_I("InteractiveMusicRadio 2.5.0  (music apps / music sites)");
	IMR_LOG_I("data dir:   %s", g_cfg.dataDir.c_str());
	IMR_LOG_I("plugin dir: %s", g_cfg.pluginDir.c_str());
	IMR_LOG_I("log file:   %s", logPath.c_str());

	const fs::path ini = fs::path(g_cfg.pluginDir) / "InteractiveMusicRadio.ini";
	g_cfg.iniPath = ini.string();
	if (g_cfg.LoadFromFile(ini.string())) {
		IMR_LOG_I("config loaded: %s", ini.string().c_str());
	} else {
		IMR_LOG_W("config not found (%s), using defaults", ini.string().c_str());
	}
	SetVerbose(g_cfg.verbose);
	// Publish the loaded binding to the atomics before any thread can read it.
	g_hotkeyKey.store(g_cfg.hotkeyKey, std::memory_order_release);
	g_hotkeyMods.store(g_cfg.hotkeyMods, std::memory_order_release);
	IMR_LOG_I("hotkey: %s", HotkeyLabel(g_hotkeyKey.load(std::memory_order_acquire),
		g_hotkeyMods.load(std::memory_order_acquire)).c_str());

	std::string err;
	if (g_handshake.Init(&err)) {
		IMR_LOG_I("CMS handshake block attached");
	} else {
		IMR_LOG_W("CMS handshake unavailable: %s", err.c_str());
	}

	g_snap.lyrics = g_cfg.lyricsOnStart;
	g_snap.hotkeyLabel = HotkeyLabel(g_cfg.hotkeyKey, g_cfg.hotkeyMods);
	g_snap.panelX = g_cfg.panelX;
	g_snap.panelY = g_cfg.panelY;
	g_snap.lyricsLocked = g_cfg.lyricLocked[0];
	ui::SetHandler(&ApplyCommand);
	ui::SetWakeNotifier(&WakeEngine);
	ui::SetMainThreadRunner([](std::function<void()> a_work) {
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask(std::move(a_work));
		} else {
			a_work();
		}
	});

	ui::Attach(g_cfg, g_snap);
	InstallHotkey();

	g_run.store(true, std::memory_order_release);
	g_engine = std::thread(EngineLoop);
	IMR_LOG_I("engine started (event-woken 16 ms frame)");
}

void StopAll()
{
	if (!g_started.exchange(false)) return;

	g_run.store(false, std::memory_order_release);
	WakeEngine();
	if (g_engine.joinable()) g_engine.join();

	RemoveHotkey();
	ui::Detach();
	for (auto& l : g_lyrics) l.Destroy();
	g_handshake.SetTakeover(false);
	g_sessions.RestoreAll();
	IMR_LOG_I("engine stopped; sessions restored; both lyric windows closed");
	LogClose();
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
			if (!a_msg) return;
			switch (a_msg->type) {
			case SKSE::MessagingInterface::kDataLoaded:
				StartAll();
				break;
			case SKSE::MessagingInterface::kPostLoadGame:
			case SKSE::MessagingInterface::kNewGame:
				InstallHotkey();   // idempotent; the manager may not exist yet
				break;
			default:
				break;
			}
		});
	}
	return true;
}

SKSEPluginInfo(
	.Version = { 2, 5, 0, 0 },
	.Name = "InteractiveMusicRadio",
	.Author = "Icarus",
	.SupportEmail = "",
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::PluginDeclaration::RuntimeCompatibility(SKSE::VersionIndependence::AddressLibrary));
