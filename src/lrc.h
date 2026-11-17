// InteractiveMusicRadio - LRC parsing (pure, offline-testable).
//
// The desktop lyric window needs one thing from a lyric file: "which line is
// playing at second N".  Everything else (fetching, drawing) lives elsewhere so
// this part can be unit tested without a network or a window.
#pragma once

#include <string>
#include <vector>

namespace imr {

struct LrcLine {
	double      at = 0.0;   // seconds from the start of the track
	std::string text;       // UTF-8, may be empty for instrumental gaps
};

// Parses "[00:12.34]text" lines, including several timestamps on one line and
// the plain "[ar:…]" metadata tags (which are skipped).  Sorted by time; empty
// text lines are kept so a gap clears the display instead of holding the last
// line for ever.
std::vector<LrcLine> ParseLrc(const std::string& a_lrc);

// The line in effect at a_pos, "" when the track has not reached the first one.
std::string LineAt(const std::vector<LrcLine>& a_lines, double a_pos);

// The next non-empty line after a_pos, for the two-line desktop-lyric look.
std::string NextLineAt(const std::vector<LrcLine>& a_lines, double a_pos);

// "title - artist" is all the lyric lookup gets; keep it here so the format
// is testable.
std::string LyricsQuery(const std::string& a_title, const std::string& a_artist);

}  // namespace imr
