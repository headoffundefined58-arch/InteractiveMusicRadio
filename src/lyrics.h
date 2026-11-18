// InteractiveMusicRadio - our own desktop lyric windows.
//
// 2.1 runs TWO independent overlays: one for a desktop player and one for a
// web player.  They have separate positions, separate lock states and separate
// lyric decks, so switching between 桌面版 / 网页版 never inherits the other's
// placement or (worse) the other's lyric.
//
// Each is a layered, always-on-top strip.  Locked = click-through (normal use);
// unlocked = receives the mouse and can be dragged anywhere on the desktop,
// exactly like the desktop-lyric widget the music apps ship.  The window is
// created on the engine thread and serviced by that thread's Pump().
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core.h"
#include "lrc.h"

namespace imr {

// Looks a synced lyric up online (best effort).  False on any failure - offline,
// service changed, track not found - and the window then shows the track line.
bool FetchLyrics(const std::string& a_title, const std::string& a_artist, std::string* a_lrc);

class LyricsOverlay {
public:
	LyricsOverlay() = default;
	~LyricsOverlay();
	LyricsOverlay(const LyricsOverlay&) = delete;
	LyricsOverlay& operator=(const LyricsOverlay&) = delete;

	// a_kind: 0 = desktop player, 1 = web player (used for its default slot on
	// screen and the "拖动定位" hint).
	bool Create(const Config& a_cfg, int a_kind);
	void Destroy();

	void SetTrack(const std::string& a_title, const std::string& a_artist,
		const std::string& a_album);
	void SetLyric(const std::vector<LrcLine>& a_lines);

	// Live subtitle line (from video OCR).  While set, Tick() shows this line
	// instead of the timed LRC, so the browser's own subtitle wins.  Callers
	// must ClearLiveLine() when the capture goes stale so the LRC takes over.
	void SetLiveLine(const std::string& a_line);
	void ClearLiveLine();
	bool HasLiveLine() const { return live_; }

	// Advance the highlight.  a_position must be the *interpolated* playback
	// position, not the last value the player reported.
	void Tick(double a_position);

	void SetVisible(bool a_on);
	bool Visible() const { return visible_; }

	// true = fixed and click-through; false = draggable.
	void SetLocked(bool a_locked);
	bool Locked() const { return locked_; }

	void SetPosition(int a_x, int a_y);       // -1 = automatic
	void GetPosition(int* a_x, int* a_y) const;
	void SetMoveCallback(std::function<void(int, int)> a_cb);

	const std::string& CurrentLine() const { return current_; }
	const std::string& NextLine() const { return next_; }

	// Drain this thread's window messages (non-blocking).  One call per engine
	// frame serves both overlays.
	void Pump();

private:
	struct Impl;
	Impl*         impl_ = nullptr;
	bool          visible_ = false;
	bool          locked_ = true;
	bool          live_ = false;      // showing a video-subtitle line
	std::string   current_;
	std::string   next_;
};

}  // namespace imr
