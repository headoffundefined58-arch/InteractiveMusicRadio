// InteractiveMusicRadio - PrismaUI bridge implementation.
//
// Written against the official modder header (extern/PrismaUI_API.h, taken from
// the PrismaUI 1.5.1 release page).  Nothing here is guessed: the interface is
// a pure-virtual C++ class obtained through one exported function, so the DLL
// keeps loading when PrismaUI is missing and picks the radio up as soon as the
// framework is present.
//
//   CreateView(path, onDomReady)   path is relative to Data\PrismaUI\views\
//   RegisterJSListener(view, name) JS calls it as window.<name>(string)
//   InteropCall(view, name, arg)   C++ calls window.<name>(arg) - no JS parsing
//
// Every call is guarded: a PrismaUI build that changes its ABI must not take the
// game down with it, and the media link itself (SMTC + handshake) lives outside
// this file and keeps working regardless.
//
// UNICODE must be defined *before* the first Windows header: PrismaUI_API.h
// calls GetModuleHandle(L"PrismaUI.dll") unqualified, and without the macro that
// resolves to the ANSI variant and fails to compile.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "ui.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "PrismaUI_API.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace imr::ui {

namespace {

using namespace PRISMA_UI_API;

IVPrismaUI1*   g_api = nullptr;    // always valid once attached
IVPrismaUI2*   g_api2 = nullptr;   // non-null only on a 1.5.1+ build
PrismaView     g_view = 0;
const Config*  g_cfg = nullptr;
Snapshot*      g_snap = nullptr;
CommandHandler  g_handler;
MainThreadRunner g_runner;   // supplied by main.cpp; owns the game-thread hop
std::function<void()> g_wake;   // wakes the engine loop when a JS command lands

std::atomic<bool> g_domReady{ false };
std::atomic<bool> g_forcePush{ false };  // DOM (re)loaded -> push even if unchanged
double    g_lastPush = -1.0;
std::string g_lastJson;      // engine thread only; avoids identical DOM pushes

// Visibility is split in two on purpose.
//
//   g_want     what the radio *should* be: written by anyone (hotkey on the game
//              thread, the ✕ button on the engine thread), read freely.
//   g_applied  what PrismaUI is actually showing: only ever touched on the game
//              thread.
//
// PrismaUI's Show/Hide drive the renderer, so calling them from the engine
// thread did nothing at all - which is exactly why the ✕ button looked dead and
// the panel could not be dismissed.  Every change is now queued onto the game
// thread and the two values are compared there.
std::atomic<bool> g_want{ false };
bool              g_applied = false;   // game thread only
std::atomic<bool> g_forceApply{ false };
ULONGLONG g_attachTick = 0;      // for the "the view never loaded" watchdog
bool      g_domWarned = false;

// Visibility bridge (defined below; declared here because OnDomReady needs it).
void QueueVisibility();

// JS callbacks can land on another thread, so they only deposit the raw
// argument here; Tick() (engine thread) turns it into an action.
std::mutex               g_mtx;
std::vector<std::string> g_inbox;

// ------------------------------------------------------------------ strings --
std::string Esc(std::string_view a_in)
{
	std::string o;
	o.reserve(a_in.size() + 8);
	for (const char c : a_in) {
		switch (c) {
		case '"':  o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n";  break;
		case '\r': o += "\\r";  break;
		case '\t': o += "\\t";  break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char b[8];
				std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(c));
				o += b;
			} else {
				o += c;
			}
		}
	}
	return o;
}

std::string F2(double a_v)
{
	char b[32];
	std::snprintf(b, sizeof(b), "%.2f", a_v);
	return b;
}

// ------------------------------------------------------------- snapshot JSON --
// One flat object per frame.  Kept hand-rolled: a JSON library would be the only
// dependency in the whole plugin, for a payload of a few hundred bytes.
std::string ToJson(const Snapshot& a_s)
{
	std::string j;
	j.reserve(512 + a_s.sources.size() * 200);

	j += "{\"state\":";
	j += std::to_string(static_cast<int>(a_s.state));
	j += ",\"takeover\":";
	j += a_s.takeover ? "1" : "0";
	j += ",\"lyrics\":";
	j += a_s.lyrics ? "1" : "0";
	j += ",\"lyric\":\"";
	j += Esc(a_s.lyricNow);
	j += "\",\"subsrc\":";
	j += std::to_string(a_s.subSource);   // 0 none / 1 video OCR / 2 network LRC
	j += ",\"active\":\"";
	j += Esc(a_s.activeId);
	j += "\",\"gain\":";
	j += F2(a_s.capturedGain);
	j += ",\"pm\":";
	j += std::to_string(a_s.playMode);   // PlayMode of the linked source, -1 = n/a
	j += ",\"px\":";
	j += std::to_string(a_s.panelX);     // panel drag offset from centre
	j += ",\"py\":";
	j += std::to_string(a_s.panelY);
	j += ",\"lk\":";
	j += a_s.lyricsLocked ? "1" : "0";   // lyric window lock (active kind)
	j += ",\"lkind\":";
	j += std::to_string(a_s.lyricsKind); // 0 desktop / 1 web
	j += ",\"bind\":";
	j += std::to_string(a_s.bindState);  // 1 = recording a hotkey
	j += ",\"offset\":";
	j += std::to_string(a_s.lyricOffsetMs);
	j += ",\"hotkey\":\"";
	j += Esc(a_s.hotkeyLabel);
	j += "\",\"seq\":";
	j += std::to_string(a_s.seq);
	j += ",\"sources\":[";

	bool first = true;
	for (const auto& s : a_s.sources) {
		if (!first) j += ',';
		first = false;
		j += "{\"id\":\"";
		j += Esc(s.id);
		j += "\",\"kind\":";
		j += std::to_string(static_cast<int>(s.kind));
		j += ",\"run\":";
		j += s.running ? "1" : "0";
		j += ",\"ok\":";
		j += s.controlled ? "1" : "0";
		j += ",\"title\":\"";
		j += Esc(s.track.title);
		j += "\",\"artist\":\"";
		j += Esc(s.track.artist);
		j += "\",\"album\":\"";
		j += Esc(s.track.album);
		j += "\",\"playing\":";
		j += s.track.playing ? "1" : "0";
		j += ",\"pos\":";
		j += F2(s.track.position);
		j += ",\"dur\":";
		j += F2(s.track.duration);
		j += ",\"vol\":";
		j += F2(static_cast<double>(s.volume));
		j += ",\"muted\":";
		j += s.muted ? "1" : "0";
		j += '}';
	}
	j += "]}";
	return j;
}

// ---------------------------------------------------------------- commands ---
// Wire format: one verb per line, then "key=value" lines, e.g.
//
//     transport
//     act=next
//
// Newline-separated on purpose: arguments can contain anything (song titles,
// session ids with backslashes, …), and only the *first* '=' on a line splits.
Command ParseCmd(const std::string& a_raw)
{
	Command c;
	bool firstTok = true;

	std::size_t pos = 0;
	while (pos <= a_raw.size()) {
		std::size_t nl = a_raw.find('\n', pos);
		if (nl == std::string::npos) nl = a_raw.size();

		std::string line = a_raw.substr(pos, nl - pos);
		while (!line.empty() &&
			(line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}

		if (firstTok) {
			c.verb = line;
			firstTok = false;
		} else if (!line.empty()) {
			const std::size_t eq = line.find('=');
			if (eq == std::string::npos) {
				c.items.emplace_back(line, std::string());
			} else {
				c.items.emplace_back(line.substr(0, eq), line.substr(eq + 1));
			}
		}
		if (nl == a_raw.size()) break;
		pos = nl + 1;
	}
	return c;
}

void OnDomReady(PrismaView a_view)
{
	g_domReady.store(true, std::memory_order_release);
	IMR_LOG_I("ui: view ready (0x%llX)", static_cast<unsigned long long>(a_view));
	// A freshly loaded page is shown by PrismaUI regardless of what we asked for
	// before it existed, so state is re-asserted here.  This is what keeps the
	// panel closed at game start.
	g_forceApply.store(true, std::memory_order_release);
	QueueVisibility();
	// This callback runs on PrismaUI's thread: never touch the snapshot or the
	// DOM from here.  Ask the engine loop for a fresh push instead.
	g_forcePush.store(true, std::memory_order_release);
	if (g_wake) g_wake();
}

void OnCommand(const char* a_arg)
{
	if (!a_arg) return;
	{
		std::lock_guard lock(g_mtx);
		// Bounded: a stuck UI cannot grow this without limit.
		if (g_inbox.size() < 64) g_inbox.emplace_back(a_arg);
	}
	// A click should not have to wait for the next engine frame.
	if (g_wake) g_wake();
}

// JS console messages (V2 only).  Without this a typo in radio.js is silent:
// the view simply stays blank and there is nothing to go on.
void OnConsole(PrismaView, ConsoleMessageLevel a_level, const char* a_msg)
{
	if (!a_msg || !*a_msg) return;
	const char* tag = "log";
	switch (a_level) {
	case ConsoleMessageLevel::Error:   tag = "error"; break;
	case ConsoleMessageLevel::Warning: tag = "warn";  break;
	case ConsoleMessageLevel::Debug:   tag = "debug"; break;
	case ConsoleMessageLevel::Info:    tag = "info";  break;
	default: break;
	}
	IMR_LOG_I("js[%s] %s", tag, a_msg);
}

// ------------------------------------------------------------- visibility ---
// Game thread only.  Doing the work here (rather than at the call site) is what
// makes the ✕ button and the Ctrl toggle actually take effect.
void ApplyVisibility()
{
	if (!g_api || !g_view) return;
	const bool want = g_want.load(std::memory_order_acquire);
	if (!g_forceApply.load(std::memory_order_acquire) && want == g_applied) return;
	g_forceApply.store(false, std::memory_order_release);

	if (want) {
		g_api->Show(g_view);
		g_api->Focus(g_view, false, false);
	} else {
		g_api->Unfocus(g_view);
		g_api->Hide(g_view);
	}
	g_applied = want;
	IMR_LOG_D("ui: %s", want ? "shown" : "hidden");
}

void QueueVisibility()
{
	if (g_runner) {
		g_runner([] { ApplyVisibility(); });
		return;
	}
	// No runner installed (only possible before StartAll): a direct call is
	// better than silently dropping the request.
	ApplyVisibility();
}

}  // namespace

// ============================================================ Command API ===

bool Command::has(const char* a_key) const
{
	for (const auto& kv : items) {
		if (kv.first == a_key) return true;
	}
	return false;
}

const std::string& Command::at(const char* a_key) const
{
	static const std::string kEmpty;
	for (const auto& kv : items) {
		if (kv.first == a_key) return kv.second;
	}
	return kEmpty;
}

bool Command::flag(const char* a_key) const
{
	const auto& v = at(a_key);
	return v == "1" || v == "true" || v == "on";
}

// ============================================================== public API ===

void PushNow()
{
	if (!g_api || !g_view ||
		!g_domReady.load(std::memory_order_acquire) || !g_snap) return;
	// InteropCall hands the payload over as a plain string argument, so the UI
	// never has to be given valid JavaScript source - no quoting bugs when a
	// song title contains an apostrophe or a newline.
	std::string json = ToJson(*g_snap);
	const bool force = g_forcePush.exchange(false, std::memory_order_acq_rel);
	if (!force && json == g_lastJson) return;   // unchanged: skip the DOM work
	g_lastJson = std::move(json);
	g_api->InteropCall(g_view, "imrUpdate", g_lastJson.c_str());
}

void SetHandler(CommandHandler a_handler)
{
	g_handler = std::move(a_handler);
}

void SetWakeNotifier(std::function<void()> a_notify)
{
	g_wake = std::move(a_notify);
}

void SetMainThreadRunner(MainThreadRunner a_runner)
{
	g_runner = std::move(a_runner);
}

bool Attach(const Config& a_cfg, Snapshot& a_snap)
{
	g_cfg = &a_cfg;
	g_snap = &a_snap;

	if (!::GetModuleHandleW(L"PrismaUI.dll")) {
		IMR_LOG_W("PrismaUI.dll not found - the visual radio is unavailable "
		          "(install PrismaUI; the media link itself still works)");
		return false;
	}

	// V2 adds the JS console callback; it derives from V1, so one pointer does
	// both jobs.  Older builds only answer V1 and the radio still works.
	if (auto* v2 = RequestPluginAPI<IVPrismaUI2>()) {
		g_api = v2;
		g_api2 = v2;
		IMR_LOG_I("PrismaUI interface acquired (V2)");
	} else {
		g_api = RequestPluginAPI<IVPrismaUI1>();
		if (!g_api) {
			IMR_LOG_W("PrismaUI is loaded but refused the V1 interface - "
			          "this build is newer/older than the header IMR was built against");
			return false;
		}
		IMR_LOG_I("PrismaUI interface acquired (V1)");
	}

	g_view = g_api->CreateView(kViewPath, &OnDomReady);
	if (!g_view) {
		IMR_LOG_E("PrismaUI CreateView failed for '%s' - "
		          "is the front-end deployed to Data\\%s ?", kViewPath, kViewDir);
		g_api = nullptr;
		g_api2 = nullptr;
		return false;
	}

	g_api->RegisterJSListener(g_view, "imrCommand", &OnCommand);
	if (g_api2) g_api2->RegisterConsoleCallback(g_view, &OnConsole);
	g_api->SetOrder(g_view, 100);

	// CreateView leaves the view *visible*.  2.0.0 therefore dropped a full-screen
	// card on the player the moment the plugin loaded - including over the main
	// menu, where it hid the Load button.  Start hidden, always: the radio is
	// something you open, never something that greets you.
	g_want.store(false, std::memory_order_release);
	g_applied = false;
	g_api->Hide(g_view);
	g_applied = false;
	if (g_api->IsHidden(g_view)) {
		IMR_LOG_I("ui: view starts hidden (the configured hotkey opens it)");
	} else {
		IMR_LOG_W("ui: Hide() had no effect - the panel may cover the screen "
		          "until the configured hotkey is pressed");
	}

	g_attachTick = ::GetTickCount64();
	g_domWarned = false;
	g_lastJson.clear();
	g_forcePush.store(false, std::memory_order_release);

	IMR_LOG_I("ui: radio view created for '%s' (the configured hotkey toggles it)",
		kViewPath);
	return true;
}

void Detach()
{
	if (g_api && g_view) {
		g_api->Destroy(g_view);
		IMR_LOG_I("ui: view destroyed");
	}
	g_view = 0;
	g_api = nullptr;
	g_api2 = nullptr;
	g_domReady.store(false, std::memory_order_release);
	g_want.store(false, std::memory_order_release);
	g_applied = false;
	g_attachTick = 0;
	g_domWarned = false;
	g_snap = nullptr;
	g_cfg = nullptr;
	g_lastJson.clear();
	g_lastPush = -1.0;
	g_forcePush.store(false, std::memory_order_release);
	{
		std::lock_guard lock(g_mtx);
		g_inbox.clear();
	}
}

bool Available()
{
	return g_api != nullptr && g_view != 0 &&
		g_domReady.load(std::memory_order_acquire);
}

void Show(bool a_show)
{
	if (!g_api || !g_view) return;
	// Idempotent on purpose: the engine thread calls this on every frame that
	// carries a command, and one task per frame would be pure waste.
	if (g_want.exchange(a_show, std::memory_order_acq_rel) == a_show) return;
	QueueVisibility();
}

bool Shown()
{
	// The intent, not the rendered state: the hotkey runs on the game thread and
	// must see the result of its own toggle immediately.
	return g_want.load(std::memory_order_acquire);
}

void Toggle()
{
	if (!g_api || !g_view) return;
	Show(!Shown());
	IMR_LOG_D("ui: toggled -> %s", Shown() ? "shown" : "hidden");
}

void Open()
{
	Show(true);
}

void Tick(double a_now, double a_interval)
{
	// A view that never reached DOM-ready is almost always a path problem:
	// PrismaUI silently keeps the (blank) view when the URL fails to load, and
	// its own log only says "File URL loading failed".  Say it once, clearly.
	if (!g_domReady.load(std::memory_order_acquire) && g_view && !g_domWarned &&
		g_attachTick && ::GetTickCount64() - g_attachTick > 5000) {
		g_domWarned = true;
		IMR_LOG_E("ui: no DOM after 5 s - PrismaUI resolves '%s' against "
		          "Data\\PrismaUI\\views\\, so the front-end must be at "
		          "Data\\%s\\", kViewPath, kViewDir);
	}

	// Commands first: the UI should react to a click before it is repainted.
	std::vector<std::string> batch;
	{
		std::lock_guard lock(g_mtx);
		if (!g_inbox.empty()) batch.swap(g_inbox);
	}
	if (!batch.empty()) {
		for (const auto& raw : batch) {
			const Command cmd = ParseCmd(raw);
			if (cmd.verb.empty()) continue;
			if (g_handler) g_handler(cmd);
		}
		PushNow();
		g_lastPush = a_now;
		return;   // one push per frame is plenty
	}

	if (g_lastPush < 0.0 || a_now - g_lastPush >= a_interval) {
		g_lastPush = a_now;
		PushNow();
	}
}

}  // namespace imr::ui
