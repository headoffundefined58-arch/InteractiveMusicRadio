// InteractiveMusicRadio - subtitle text cleanup + stabiliser (pure logic).
// No Win32 / WinRT here so the decision logic can be unit-tested offline.
#include "subtitle.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace imr {
namespace {

bool IsSpace(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
	       c == '\v' || c == '\f';
}

// A line made only of separators / decoration carries no subtitle text.
bool IsNoiseLine(const std::string& a_s)
{
	if (a_s.empty()) return true;
	int letters = 0;
	for (unsigned char c : a_s) {
		if (std::isalnum(c)) ++letters;
		// UTF-8 CJK/hiragana/hangul bytes are >= 0x80 and count as content.
		else if (c >= 0x80) ++letters;
	}
	// "♪♪♪", "——", "|||" -> noise.  A single real character still passes.
	return letters == 0;
}

std::string Trim(const std::string& a_s)
{
	std::size_t b = 0, e = a_s.size();
	while (b < e && IsSpace(static_cast<unsigned char>(a_s[b]))) ++b;
	while (e > b && IsSpace(static_cast<unsigned char>(a_s[e - 1]))) --e;
	return a_s.substr(b, e - b);
}

// Collapse runs of whitespace inside a line to one space.
std::string Collapse(const std::string& a_s)
{
	std::string out;
	out.reserve(a_s.size());
	bool inSpace = false;
	for (unsigned char c : a_s) {
		if (IsSpace(c)) {
			if (!inSpace) out.push_back(' ');
			inSpace = true;
		} else {
			out.push_back(static_cast<char>(c));
			inSpace = false;
		}
	}
	return Trim(out);
}

}  // namespace

std::string CleanOcrText(const std::vector<std::string>& a_lines)
{
	std::string joined;
	for (const auto& raw : a_lines) {
		const std::string line = Collapse(raw);
		if (!IsNoiseLine(line)) {
			if (!joined.empty()) joined += " ";
			joined += line;
		}
	}
	return joined;
}

std::string SubtitleStabilizer::Feed(std::string a_clean)
{
	const std::string clean = Trim(std::move(a_clean));
	if (clean.empty()) {
		Miss();
		return {};
	}

	if (clean == published_) {
		// Same line still on screen: keep the candidate in sync so a real
		// change does not have to fight a stale candidate.
		candidate_ = clean;
		hitsSeen_ = hits_;
		return {};
	}

	if (clean == candidate_) {
		++hitsSeen_;
	} else {
		candidate_ = clean;
		hitsSeen_ = 1;
	}

	if (hitsSeen_ >= hits_) {
		published_ = candidate_;
		candidate_.clear();
		hitsSeen_ = 0;
		return published_;
	}
	return {};
}

void SubtitleStabilizer::Miss()
{
	candidate_.clear();
	hitsSeen_ = 0;
}

void SubtitleStabilizer::Reset()
{
	candidate_.clear();
	hitsSeen_ = 0;
	published_.clear();
}

}  // namespace imr
