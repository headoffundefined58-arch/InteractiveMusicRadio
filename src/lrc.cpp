// InteractiveMusicRadio - LRC parsing implementation.
#include "lrc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace imr {

namespace {

// "[mm:ss.xx]" / "[mm:ss:xx]" / "[mm:ss]" -> seconds.  Returns false when the
// bracket does not hold a timestamp (a metadata tag such as "[ti:…]").
bool ReadStamp(const std::string& a_body, double* a_out)
{
	std::size_t i = 0;
	int mm = 0;
	while (i < a_body.size() && std::isdigit(static_cast<unsigned char>(a_body[i]))) {
		mm = mm * 10 + (a_body[i] - '0');
		++i;
	}
	if (i == 0 || i >= a_body.size() || a_body[i] != ':') return false;
	++i;

	int ss = 0;
	std::size_t digits = 0;
	while (i < a_body.size() && std::isdigit(static_cast<unsigned char>(a_body[i]))) {
		ss = ss * 10 + (a_body[i] - '0');
		++i;
		++digits;
	}
	if (digits == 0) return false;

	double frac = 0.0;
	if (i < a_body.size() && (a_body[i] == '.' || a_body[i] == ':')) {
		++i;
		double scale = 0.1;
		while (i < a_body.size() && std::isdigit(static_cast<unsigned char>(a_body[i]))) {
			frac += (a_body[i] - '0') * scale;
			scale *= 0.1;
			++i;
		}
	}
	*a_out = mm * 60.0 + ss + frac;
	return true;
}

std::string Trim(const std::string& a_s)
{
	std::size_t b = 0;
	std::size_t e = a_s.size();
	while (b < e && (a_s[b] == ' ' || a_s[b] == '\t' || a_s[b] == '\r')) ++b;
	while (e > b && (a_s[e - 1] == ' ' || a_s[e - 1] == '\t' || a_s[e - 1] == '\r')) --e;
	return a_s.substr(b, e - b);
}

}  // namespace

std::vector<LrcLine> ParseLrc(const std::string& a_lrc)
{
	std::vector<LrcLine> out;
	std::size_t pos = 0;

	while (pos < a_lrc.size()) {
		std::size_t nl = a_lrc.find('\n', pos);
		if (nl == std::string::npos) nl = a_lrc.size();
		const std::string raw = a_lrc.substr(pos, nl - pos);
		pos = nl + 1;

		std::vector<double> stamps;
		std::size_t i = 0;
		while (i < raw.size() && raw[i] == '[') {
			const std::size_t close = raw.find(']', i);
			if (close == std::string::npos) break;
			double t = 0.0;
			if (!ReadStamp(raw.substr(i + 1, close - i - 1), &t)) break;
			stamps.push_back(t);
			i = close + 1;
		}
		if (stamps.empty()) continue;   // metadata or junk

		const std::string text = Trim(raw.substr(i));
		for (const double t : stamps) {
			out.push_back(LrcLine{ t, text });
		}
	}

	std::stable_sort(out.begin(), out.end(),
		[](const LrcLine& a, const LrcLine& b) { return a.at < b.at; });
	return out;
}

std::string LineAt(const std::vector<LrcLine>& a_lines, double a_pos)
{
	if (a_lines.empty() || a_pos < 0.0) return {};
	std::string cur;
	for (const auto& l : a_lines) {
		if (l.at > a_pos) break;
		cur = l.text;   // empty lines clear the display, by design
	}
	return cur;
}

std::string NextLineAt(const std::vector<LrcLine>& a_lines, double a_pos)
{
	for (const auto& l : a_lines) {
		if (l.at > a_pos && !l.text.empty()) return l.text;
	}
	return {};
}

std::string LyricsQuery(const std::string& a_title, const std::string& a_artist)
{
	if (a_title.empty()) return {};
	if (a_artist.empty()) return a_title;
	return a_title + " - " + a_artist;
}

}  // namespace imr
