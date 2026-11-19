// InteractiveMusicRadio - shared logic implementation (logging, config, helpers).
#include "core.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace imr {

double NowSeconds()
{
	using clock = std::chrono::steady_clock;
	static const auto t0 = clock::now();
	return std::chrono::duration<double>(clock::now() - t0).count();
}

// ------------------------------------------------------------------ logging
namespace {
std::FILE*  g_log = nullptr;
std::mutex  g_logMtx;
bool        g_verbose = false;
}

void LogOpen(const std::string& a_path)
{
	// Close any previous handle *before* taking the lock: LogClose() locks the
	// same non-recursive mutex, so calling it while holding g_logMtx deadlocks.
	LogClose();
	std::lock_guard<std::mutex> lk(g_logMtx);
	g_log = std::fopen(a_path.c_str(), "w");
}

void LogClose()
{
	std::lock_guard<std::mutex> lk(g_logMtx);
	if (g_log) {
		std::fflush(g_log);
		std::fclose(g_log);
		g_log = nullptr;
	}
}

void SetVerbose(bool a_on)
{
	std::lock_guard<std::mutex> lk(g_logMtx);
	g_verbose = a_on;
}

void LogMsg(const char* a_level, const char* a_fmt, ...)
{
	(void)g_verbose;
	va_list args;
	va_start(args, a_fmt);
	std::lock_guard<std::mutex> lk(g_logMtx);
	if (!g_log) { va_end(args); return; }
	std::fprintf(g_log, "[%s] ", a_level);
	std::vfprintf(g_log, a_fmt, args);
	std::fprintf(g_log, "\n");
	std::fflush(g_log);
	va_end(args);
}

void LogFlush()
{
	std::lock_guard<std::mutex> lk(g_logMtx);
	if (g_log) std::fflush(g_log);
}

// ------------------------------------------------------------------- config
namespace {
std::string Trim(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
	return std::string(s);
}
}

void Config::ApplyKey(std::string_view a_key, std::string_view a_value)
{
	const std::string v = Trim(a_value);
	const std::string k = Trim(a_key);

	auto asBool = [&](bool& out) {
		if (v == "1" || v == "true" || v == "True" || v == "yes") out = true;
		else if (v == "0" || v == "false" || v == "False" || v == "no") out = false;
	};
	auto asFloat = [&](float& out) {
		if (!v.empty()) { try { out = std::stof(v); } catch (...) {} }
	};
	auto asInt = [&](int& out) {
		if (!v.empty()) { try { out = std::stoi(v); } catch (...) {} }
	};

	if (k == "bEnabled") asBool(enabled);
	else if (k == "bDuckInDialogue") asBool(duckInDialogue);
	else if (k == "fDialogueDuck") asFloat(dialogueDuck);
	else if (k == "fLocalDuck") asFloat(localDuck);
	else if (k == "iSessionPollMs") asInt(sessionPollMs);
	else if (k == "iTrackPollMs") asInt(trackPollMs);
	else if (k == "iHotkeyKey") asInt(hotkeyKey);
	else if (k == "iHotkeyMods") asInt(hotkeyMods);
	else if (k == "iOpenKey") {          // legacy ini: treat as the hotkey
		int legacy = 0;
		asInt(legacy);
		if (legacy > 0) { hotkeyKey = legacy; hotkeyMods = 0; }
	}
	else if (k == "bCtrlInSystemMenu") asBool(ctrlInSystemMenu);  // ignored
	else if (k == "iPanelX") asInt(panelX);
	else if (k == "iPanelY") asInt(panelY);
	else if (k == "iLyricDesktopX") asInt(lyricX[0]);
	else if (k == "iLyricDesktopY") asInt(lyricY[0]);
	else if (k == "iLyricWebX") asInt(lyricX[1]);
	else if (k == "iLyricWebY") asInt(lyricY[1]);
	else if (k == "bLyricDesktopLocked") asBool(lyricLocked[0]);
	else if (k == "bLyricWebLocked") asBool(lyricLocked[1]);
	else if (k == "bLyricsOnStart") asBool(lyricsOnStart);
	else if (k == "bFetchLyrics") asBool(fetchLyrics);
	else if (k == "iLyricFontSize") asInt(lyricFontSize);
	else if (k == "iLyricOffsetMs") asInt(lyricOffsetMs);
	else if (k == "bVerbose") asBool(verbose);
}

bool Config::LoadFromFile(const std::string& a_path)
{
	std::ifstream in(a_path);
	if (!in) return false;
	std::string line;
	while (std::getline(in, line)) {
		// ';' starts a comment; sections [Foo] are ignored like CMS.
		auto hash = line.find(';');
		if (hash != std::string::npos) line = line.substr(0, hash);
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		ApplyKey(std::string_view(line).substr(0, eq),
		         std::string_view(line).substr(eq + 1));
	}
	iniPath = a_path;
	return true;
}

bool Config::SaveToFile(const std::string& a_path) const
{
	std::ofstream out(a_path);
	if (!out) return false;
	out << "; InteractiveMusicRadio configuration - written by the in-game panel.\n";
	out << "; ';' starts a comment; [sections] are ignored.  All keys can be\n";
	out << "; edited by hand while the game is closed.\n\n";
	out << "bEnabled=" << (enabled ? 1 : 0) << "\n";
	out << "bDuckInDialogue=" << (duckInDialogue ? 1 : 0) << "\n";
	out << "fDialogueDuck=" << dialogueDuck << "\n";
	out << "fLocalDuck=" << localDuck << "\n";
	out << "iSessionPollMs=" << sessionPollMs << "\n";
	out << "iTrackPollMs=" << trackPollMs << "\n\n";
	out << "; Launcher hotkey.  Recorded in-game (settings row).  Scan codes:\n";
	out << "; F1=59 ... F10=68, F11=87, F12=88, Esc=1, Space=57, Enter=28.\n";
	out << "; iHotkeyMods: 1=Ctrl 2=Shift 4=Alt (add them together).\n";
	out << "iHotkeyKey=" << hotkeyKey << "\n";
	out << "iHotkeyMods=" << hotkeyMods << "\n\n";
	out << "; Panel drag offset from the centred default, in pixels.\n";
	out << "iPanelX=" << panelX << "\n";
	out << "iPanelY=" << panelY << "\n\n";
	out << "; Two independent lyric windows: [desktop app] and [web player].\n";
	out << "; -1 = automatic position (bottom centre).  Locked = click-through.\n";
	out << "iLyricDesktopX=" << lyricX[0] << "\n";
	out << "iLyricDesktopY=" << lyricY[0] << "\n";
	out << "iLyricWebX=" << lyricX[1] << "\n";
	out << "iLyricWebY=" << lyricY[1] << "\n";
	out << "bLyricDesktopLocked=" << (lyricLocked[0] ? 1 : 0) << "\n";
	out << "bLyricWebLocked=" << (lyricLocked[1] ? 1 : 0) << "\n\n";
	out << "bLyricsOnStart=" << (lyricsOnStart ? 1 : 0) << "\n";
	out << "bFetchLyrics=" << (fetchLyrics ? 1 : 0) << "\n";
	out << "iLyricFontSize=" << lyricFontSize << "\n";
	out << "iLyricOffsetMs=" << lyricOffsetMs << "\n";
	out << "bVerbose=" << (verbose ? 1 : 0) << "\n";
	return static_cast<bool>(out);
}

// --------------------------------------------------------------- helpers ---
LinkState NextLinkState(LinkState a_cur, bool a_haveSources, bool a_selectedActive)
{
	if (a_selectedActive && a_haveSources) return LinkState::Linked;
	if (a_haveSources) return LinkState::Listening;
	return LinkState::Disconnected;
}

PlayMode PlayModeOf(int a_shuffle, int a_repeat)
{
	// A player that reports neither switch has no opinion; saying "顺序播放"
	// for it would be a lie, and the menu would then highlight the wrong row.
	if (a_shuffle < 0 || a_repeat < 0) return PlayMode::Unknown;
	if (a_shuffle != 0) return PlayMode::Shuffle;
	switch (a_repeat) {
	case 1:  return PlayMode::RepeatOne;
	case 2:  return PlayMode::RepeatAll;
	case 0:  return PlayMode::Sequential;
	default: return PlayMode::Unknown;
	}
}

std::string KeyName(int a_code)
{
	if (a_code <= 0) return "未设置";
	if (a_code >= 59 && a_code <= 68) return "F" + std::to_string(a_code - 58);
	if (a_code == 87) return "F11";
	if (a_code == 88) return "F12";

	static const char* const kDigits[] = {
		"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"
	};
	if (a_code >= 2 && a_code <= 11) return kDigits[a_code - 2];

	static const char* const kRows[] = {
		"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
	};
	static const int kStart[] = { 16, 30, 44 };
	for (int r = 0; r < 3; ++r) {
		const int n = static_cast<int>(std::strlen(kRows[r]));
		if (a_code >= kStart[r] && a_code < kStart[r] + n) {
			return std::string(1, kRows[r][a_code - kStart[r]]);
		}
	}

	switch (a_code) {
	case 1:   return "Esc";
	case 15:  return "Tab";
	case 28:  return "Enter";
	case 57:  return "Space";
	case 58:  return "CapsLock";
	case 29:  return "Ctrl";
	case 42:  return "Shift";
	case 56:  return "Alt";
	case 199: return "Home";
	case 200: return "↑";
	case 203: return "←";
	case 205: return "→";
	case 207: return "End";
	case 208: return "↓";
	case 210: return "Insert";
	case 211: return "Delete";
	default:  break;
	}
	return "键" + std::to_string(a_code);
}

std::string HotkeyLabel(int a_key, int a_mods)
{
	if (a_key <= 0) return "未设置";
	std::string out;
	if (a_mods & 1) out += "Ctrl + ";
	if (a_mods & 2) out += "Shift + ";
	if (a_mods & 4) out += "Alt + ";
	return out + KeyName(a_key);
}

PlayMode PlayModeOfLoose(int a_shuffle, int a_repeat)
{
	if (a_shuffle >= 0 && a_repeat >= 0) return PlayModeOf(a_shuffle, a_repeat);
	if (a_shuffle > 0) return PlayMode::Shuffle;          // shuffle always wins
	if (a_repeat >= 0) {
		switch (a_repeat) {
		case 0:  return PlayMode::Sequential;
		case 1:  return PlayMode::RepeatOne;
		case 2:  return PlayMode::RepeatAll;
		default: return PlayMode::Unknown;
		}
	}
	if (a_shuffle == 0) return PlayMode::Sequential;      // "not shuffled" only
	return PlayMode::Unknown;
}

double PositionNow(double a_position, double a_positionAt, bool a_playing,
	double a_now, double a_duration)
{
	double pos = a_position;
	if (a_playing && a_positionAt > 0.0 && a_now > a_positionAt) {
		const double dt = a_now - a_positionAt;
		// A sample older than 10 s is stale (the app was paused / restarted);
		// extrapolating it would send the bar far past the end of the track.
		if (dt < 10.0) pos += dt;
	}
	if (a_duration > 1.0) return std::max(0.0, std::min(pos, a_duration));
	return std::max(0.0, pos);
}

void PlayModeSwitches(PlayMode a_mode, bool* a_shuffle, int* a_repeat)
{
	// 随机播放 still has to keep the queue alive, or it would stop after one
	// pass through the shuffled list - hence list repeat for both 0 and 3.
	switch (a_mode) {
	case PlayMode::Shuffle:    if (a_shuffle) *a_shuffle = true;  if (a_repeat) *a_repeat = 2; break;
	case PlayMode::Sequential: if (a_shuffle) *a_shuffle = false; if (a_repeat) *a_repeat = 0; break;
	case PlayMode::RepeatOne:  if (a_shuffle) *a_shuffle = false; if (a_repeat) *a_repeat = 1; break;
	case PlayMode::RepeatAll:  if (a_shuffle) *a_shuffle = false; if (a_repeat) *a_repeat = 2; break;
	default:
		if (a_shuffle) *a_shuffle = false;
		if (a_repeat) *a_repeat = 0;
		break;
	}
}

}  // namespace imr
