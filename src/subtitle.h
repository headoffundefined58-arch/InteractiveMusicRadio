// InteractiveMusicRadio - video subtitle capture (header).
//
// Feature: when a WEB player is linked and the desktop lyric switch is on,
// IMR grabs frames of the browser window that is playing the video and runs
// Windows OCR over the bottom strip where subtitles are drawn.  The recognised
// line replaces the network lyric until the capture stops or goes stale, at
// which point the network lyric takes over again.
//
// The file is split in two layers:
//   * pure text helpers (CleanOcrText / SubtitleStabilizer) - no WinRT, fully
//     offline-testable, live in subtitle_text.cpp;
//   * VideoSubtitle - owns a worker thread with the WinRT Graphics Capture +
//     OCR pipeline, in video_capture.cpp.  The engine thread only calls
//     Start / Stop / Take, none of which block on WinRT.
//
// Why Graphics Capture (WGC) and not BitBlt/PrintWindow: the game is fullscreen
// and the browser is behind it.  WGC captures the window's own surface even
// while occluded and reads GPU-composited video frames, which GDI copies miss.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core.h"

namespace imr {

// ----------------------------------------------------------- pure helpers ---
// Collapse whitespace, drop OCR noise (isolated punctuation / garbage glyphs),
// and join the recognised lines into one displayable subtitle line.
// Empty result means "nothing worth showing this frame".
[[nodiscard]] std::string CleanOcrText(const std::vector<std::string>& a_lines);

// Turns noisy per-frame OCR into a stable line.  A candidate must be seen N
// consecutive times before it is published, so single-frame misreads never
// flash on the lyric window.  Identical repeats of the published line keep it
// alive (a subtitle stays on screen for several seconds).
class SubtitleStabilizer {
public:
	explicit SubtitleStabilizer(int a_hits = 2) : hits_(a_hits < 1 ? 2 : a_hits) {}

	// Feed one frame's cleaned text.  Returns the line to publish, or "" when
	// the frame produced nothing new (caller keeps the previous line).
	std::string Feed(std::string a_clean);

	// Drop the candidate without touching the published line (frame gap).
	void Miss();

	void Reset();
	const std::string& Published() const { return published_; }

private:
	int         hits_ = 2;
	std::string candidate_;
	int         hitsSeen_ = 0;
	std::string published_;
};

// ------------------------------------------------------------- capture ------
struct VideoSubConfig {
	bool enabled = true;
	int  intervalMs = 400;      // OCR cadence; subtitles change ~1x/second
	int  cropBottomPct = 38;    // bottom strip of the video that holds subtitles
	int  minWidth = 320;        // ignore too-small browser windows
};

// Owns the WGC + OCR worker.  Safe to construct without WinRT; Init() probes
// for a usable OCR language pack and Start() spins up the capture thread.
class VideoSubtitle {
public:
	VideoSubtitle() = default;
	~VideoSubtitle();
	VideoSubtitle(const VideoSubtitle&) = delete;
	VideoSubtitle& operator=(const VideoSubtitle&) = delete;

	// Probe OCR language availability.  Call once on the engine thread.
	// Returns false and fills a_err when no recogniser language is installed.
	bool Init(std::string* a_err);

	// Start capturing the browser window that currently owns a media session.
	// a_hintTitle is the current track title (used to pick the right window
	// when several browsers are open).  Idempotent: restarts if the target
	// window changed.  Returns false (and logs) when capture cannot start.
	bool Start(const VideoSubConfig& a_cfg, const std::string& a_hintTitle);
	void Stop();

	bool Running() const;
	bool Available() const { return available_; }

	// Take the freshest accepted subtitle since the last call.  Non-blocking.
	// a_at receives the steady-clock stamp of that recognition.
	bool Take(std::string* a_text, double* a_at);

private:
	struct Impl;
	Impl* impl_ = nullptr;
	bool  available_ = false;
};

}  // namespace imr
