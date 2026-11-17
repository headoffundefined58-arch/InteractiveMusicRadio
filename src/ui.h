// InteractiveMusicRadio - the in-game radio UI, rendered by PrismaUI.
//
// PrismaUI is a *runtime* dependency, not a build dependency.  Its public
// modder header (PrismaUI_API.h, shipped on its release page) declares the
// versioned interface and resolves everything through
//
//     GetModuleHandle(L"PrismaUI.dll") -> GetProcAddress("RequestPluginAPI")
//
// so IMR never links against it, never needs vcpkg or the Ultralight SDK, and
// simply does without the visual radio when the framework is not installed.
//
// This file owns the bridge only.  Commands are decoded here and handed to the
// runtime through a handler, so the UI layer never touches SMTC or the
// lyric window - it just moves strings.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core.h"

namespace imr::ui {

// One decoded command: "select\nid=qqmusic", "transport\nact=next", …
struct Command {
	std::string verb;
	std::vector<std::pair<std::string, std::string>> items;

	bool has(const char* a_key) const;
	const std::string& at(const char* a_key) const;
	bool flag(const char* a_key) const;
};

using CommandHandler = std::function<void(const Command&)>;

// Runs a task on the game thread.  PrismaUI's Show/Hide drive the renderer, so
// they must not be called from the engine thread - but this layer is kept free
// of SKSE headers (it is part of the testable core), so main.cpp installs its
// task-interface wrapper here.
using MainThreadRunner = std::function<void(std::function<void()>)>;
void SetMainThreadRunner(MainThreadRunner a_runner);

// Optional: called on the PrismaUI thread whenever a JS command arrives, so
// the engine loop can wake up immediately instead of waiting for its next
// frame.  Installed once at startup, then only read.
void SetWakeNotifier(std::function<void()> a_notify);

// Called once the data is loaded (PrismaUI must already be in the process).
// Returns false when PrismaUI is absent or refuses the interface - the plugin
// still runs, just without the visual radio.
bool Attach(const Config& a_cfg, Snapshot& a_snap);

// Destroy the view and drop the interface.  Safe to call when never attached.
void Detach();

// True while a live view exists.
bool Available();

// Where commands go; called on the engine thread.
void SetHandler(CommandHandler a_handler);

// Show/hide + focus the radio.
void Toggle();
void Open();
void Show(bool a_show);
bool Shown();

// Drive one frame of the bridge: apply queued JS commands, then push the
// snapshot at `a_interval` seconds (10 Hz keeps the progress bar smooth).
// Identical payloads are skipped, so an idle radio costs no DOM work.
// Call from the engine thread.
void Tick(double a_now, double a_interval = 0.10);

// Push the snapshot immediately (after a command, so the UI feels instant).
void PushNow();

// The view's HTML.  PrismaUI does NOT resolve this against Data\ - it prepends
// "file:///views/" and uses Data\PrismaUI\views\ as the base directory (see
// https://www.prismaui.dev/api/create-view/), so the front-end must live in
//
//     Data\PrismaUI\views\InteractiveMusicRadio\index.html
constexpr const char* kViewPath = "InteractiveMusicRadio/index.html";

// Where the front-end has to be deployed, relative to Data - used for the
// "it never loaded" diagnostics so the log states the real requirement.
constexpr const char* kViewDir = "PrismaUI\\views\\InteractiveMusicRadio";

}  // namespace imr::ui
