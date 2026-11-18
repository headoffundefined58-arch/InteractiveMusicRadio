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
// Bytes >= 0x80 are NOT automatically content: UTF-8 punctuation and symbols
// (— U+2014, ♪ U+266A, ...) also have high bytes.  Decode proper code points
// and ignore the punctuation / symbol ranges a subtitle renderer draws as
// decoration, while keeping CJK, kana, hangul and other real scripts.
std::size_t Utf8Codepoint(const std::string& a_s, std::size_t a_i, unsigned int& a_cp)
{
	const unsigned char b0 = static_cast<unsigned char>(a_s[a_i]);
	if (b0 < 0x80) {
		a_cp = b0;
		return 1;
	}
	unsigned int cp = 0;
	std::size_t n = 0;
	if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; n = 2; }
	else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; n = 3; }
	else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; n = 4; }
	else { a_cp = b0; return 1; }  // stray continuation byte
	if (a_i + n > a_s.size()) { a_cp = b0; return 1; }
	for (std::size_t k = 1; k < n; ++k) {
		const unsigned char bk = static_cast<unsigned char>(a_s[a_i + k]);
		if ((bk & 0xC0) != 0x80) { a_cp = b0; return 1; }
		cp = (cp << 6) | (bk & 0x3F);
	}
	a_cp = cp;
	return n;
}

bool IsDecoration(unsigned int a_cp)
{
	if (a_cp < 0x80) return std::isalnum(static_cast<unsigned char>(a_cp)) == 0;
	// General punctuation, arrows/math/dingbats, CJK punctuation, fullwidth
	// punctuation.  Everything else (CJK, kana, hangul, Latin ext, emoji
	// used as genuine subtitle text) is treated as content.
	return (a_cp >= 0x2000 && a_cp <= 0x206F) ||
	       (a_cp >= 0x2190 && a_cp <= 0x2BFF) ||
	       (a_cp >= 0xFE10 && a_cp <= 0xFE6F) ||
	       (a_cp >= 0xFF01 && a_cp <= 0xFF0F) ||
	       (a_cp >= 0xFF1A && a_cp <= 0xFF20) ||
	       (a_cp >= 0xFF3B && a_cp <= 0xFF40) ||
	       (a_cp >= 0xFF5B && a_cp <= 0xFF65);
}

bool IsNoiseLine(const std::string& a_s)
{
	if (a_s.empty()) return true;
	int letters = 0;
	for (std::size_t i = 0; i < a_s.size(); ) {
		unsigned int cp = 0;
		i += Utf8Codepoint(a_s, i, cp);
		if (!IsDecoration(cp)) ++letters;
	}
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
