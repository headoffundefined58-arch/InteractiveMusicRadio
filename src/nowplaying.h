// InteractiveMusicRadio - now-playing via the Windows media session (SMTC).
//
// This is the piece that makes "控制后台挂起的音乐软件" possible without touching
// another process: every modern player (QQ音乐, 网易云音乐, 汽水音乐, and any
// browser playing a web player) publishes a media session to the OS.  Windows
// exposes it through GlobalSystemMediaTransportControlsSessionManager:
//
//   * metadata  - title / artist / album, even when the app is minimised;
//   * transport - play / pause / next / previous;
//   * timeline  - position and duration, used to scroll our lyrics.
//
// Nothing is injected, nothing is hooked, and the API is the same one the
// Windows volume flyout uses, so behaviour matches what the player's own keys
// do.
#pragma once

#include <string>
#include <vector>

#include "core.h"

namespace imr {

// One OS media session, already reduced to plain strings so the rest of the
// plugin never sees a WinRT type.
struct SessionInfo {
	std::string aumid;   // "QQMusic.exe", "Chrome", …
	std::string title;
	std::string artist;
	std::string album;
	bool        playing = false;
	double      position = 0.0;   // seconds, corrected to the sampling instant
	double      positionAt = 0.0; // NowSeconds() when position was sampled
	double      duration = 0.0;
	int         shuffle = -1;   // -1 = the player does not publish this switch
	int         repeat = -1;    // 0 none / 1 track / 2 list (-1 = unknown)
};

class NowPlaying {
public:
	NowPlaying() = default;
	~NowPlaying();
	NowPlaying(const NowPlaying&) = delete;
	NowPlaying& operator=(const NowPlaying&) = delete;

	// Safe to call from the engine thread; initialises WinRT there (MTA) on the
	// first call so the apartment matches the thread that will use it.
	bool Init(std::string* a_err);

	// Snapshot every session.  Returns false when the platform has no such
	// manager (very old Windows) - the radio then works on window titles only.
	bool Poll(std::vector<SessionInfo>& a_out);

	// a_act: "play" | "pause" | "toggle" | "next" | "prev"
	bool Command(const std::string& a_aumid, const char* a_act);

	// 随机 / 顺序 / 单曲 / 列表.  Two SMTC switches set independently, because a
	// player may implement one and not the other; the log records which half
	// took effect.  a_mode is a PlayMode.
	bool SetPlayMode(const std::string& a_aumid, PlayMode a_mode);

	// Jump to a_seconds (drag on the progress bar).  SMTC takes 100 ns ticks.
	bool Seek(const std::string& a_aumid, double a_seconds);

	// The aumid of the session that is currently playing, "" when silent.
	std::string PlayingAumid();

	bool Available() const { return ready_; }

private:
	struct Impl;
	Impl* impl_ = nullptr;
	bool  ready_ = false;
	bool  tried_ = false;
};

}  // namespace imr
