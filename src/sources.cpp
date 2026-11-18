// InteractiveMusicRadio - the source catalogue implementation.
#include "sources.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace imr {

namespace {

std::string Lower(std::string a_s)
{
	for (auto& c : a_s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return a_s;
}

bool Has(const std::string& a_hay, const char* a_needle)
{
	return a_hay.find(a_needle) != std::string::npos;
}

// Desktop programs.  The exe names are the ones the vendors ship; the aumid
// fragments are what SMTC reports, which is often *not* the exe name
// (Electron apps report their product name).  Titles are the last resort when
// both are unusual - a localised build, a portable copy.
struct DesktopProbe {
	const char*        id;
	const char* const* exes;
	std::size_t        exeCount;
	const char* const* aumids;
	std::size_t        aumidCount;
	const char* const* titles;
	std::size_t        titleCount;
};

const char* const kQqExes[]   = { "qqmusic.exe", "qqmusiclite.exe", "qqmusic_recom.exe" };
const char* const kQqAumids[] = { "qqmusic", "tencent.qqmusic" };
const char* const kQqTitles[] = { "qq音乐" };

const char* const kNeExes[]   = { "cloudmusic.exe", "neteasecloudmusic.exe" };
const char* const kNeAumids[] = { "cloudmusic", "netease" };
const char* const kNeTitles[] = { "网易云音乐" };

// 汽水音乐 is a ByteDance product and has shipped under a few internal names;
// listing them all costs nothing and saves a round trip when the vendor renames
// the binary again.
const char* const kSodaExes[]   = { "sodamusic.exe", "qishui.exe", "lunamusic.exe",
                                    "dongdongmusic.exe", "sodamusicapp.exe" };
const char* const kSodaAumids[] = { "soda", "qishui", "luna" };
const char* const kSodaTitles[] = { "汽水音乐", "汽水" };

// Matched as substrings *without* the extension: SMTC reports a product name
// ("Chrome"), not an executable ("chrome.exe"), and both have to classify.
const char* const kBrowserExes[] = {
	"chrome", "msedge", "firefox", "brave", "opera", "vivaldi",
	"360se", "360chrome", "qqbrowser", "sogouexplorer", "liebao", "maxthon",
	// Standalone video/music clients that render their subtitles inside the
	// window just like a browser tab.  Without these, WGC never finds the
	// window when the user watches a video in an app instead of a browser.
	"bilibili", "bili", "iqiyi", "youku", "mgtv", "kugou", "kuwo"
};

const std::array<DesktopProbe, 3> kDesktop{ {
	{ "qqmusic", kQqExes, std::size(kQqExes), kQqAumids, std::size(kQqAumids),
	  kQqTitles, std::size(kQqTitles) },
	{ "netease", kNeExes, std::size(kNeExes), kNeAumids, std::size(kNeAumids),
	  kNeTitles, std::size(kNeTitles) },
	{ "soda", kSodaExes, std::size(kSodaExes), kSodaAumids, std::size(kSodaAumids),
	  kSodaTitles, std::size(kSodaTitles) },
} };

bool MatchesAny(const std::string& a_lower, const char* const* a_list, std::size_t a_n)
{
	for (std::size_t i = 0; i < a_n; ++i) {
		if (a_lower.find(a_list[i]) != std::string::npos) return true;
	}
	return false;
}

}  // namespace

const std::vector<SourceDef>& Catalog()
{
	static const std::vector<SourceDef> kCatalog{
		{ "qqmusic",     SourceKind::Desktop, "https://y.qq.com/" },
		{ "netease",     SourceKind::Desktop, "https://music.163.com/" },
		{ "soda",        SourceKind::Desktop, "https://qishui.douyin.com/" },
		{ "web-qqmusic", SourceKind::Web,     "https://y.qq.com/" },
		{ "web-netease", SourceKind::Web,     "https://music.163.com/" },
		{ "web-soda",    SourceKind::Web,     "https://qishui.douyin.com/" },
	};
	return kCatalog;
}

const SourceDef* FindSource(const std::string& a_id)
{
	for (const auto& d : Catalog()) {
		if (a_id == d.id) return &d;
	}
	return nullptr;
}

std::string ExeBaseName(const std::string& a_path)
{
	std::string s = a_path;
	const auto cut = s.find_last_of("\\/");
	if (cut != std::string::npos) s.erase(0, cut + 1);
	// session identifiers arrive as "…\QQMusic.exe|1234"; drop the suffix
	const auto bar = s.find('|');
	if (bar != std::string::npos) s.erase(bar);
	return Lower(s);
}

bool IsBrowserExe(const std::string& a_exe)
{
	return MatchesAny(Lower(a_exe), kBrowserExes, std::size(kBrowserExes));
}

const SourceDef* ClassifyApp(const std::string& a_exe, const std::string& a_aumid,
    const std::string& a_title)
{
	const std::string exe = Lower(a_exe);
	const std::string aum = Lower(a_aumid);
	const std::string title = Lower(a_title);

	for (const auto& p : kDesktop) {
		if (!exe.empty() && MatchesAny(exe, p.exes, p.exeCount)) return FindSource(p.id);
	}
	for (const auto& p : kDesktop) {
		if (!aum.empty() && MatchesAny(aum, p.aumids, p.aumidCount)) return FindSource(p.id);
	}
	for (const auto& p : kDesktop) {
		if (!title.empty() && MatchesAny(title, p.titles, p.titleCount)) return FindSource(p.id);
	}
	return nullptr;
}

std::string TrimCopy(const std::string& a_s)
{
	std::size_t b = 0;
	std::size_t e = a_s.size();
	while (b < e && (a_s[b] == ' ' || a_s[b] == '\t')) ++b;
	while (e > b && (a_s[e - 1] == ' ' || a_s[e - 1] == '\t')) --e;
	return a_s.substr(b, e - b);
}

void NormalizeTrack(const std::string& a_title, const std::string& a_artist,
	std::string* a_song, std::string* a_artistOut)
{
	static const char* const kSiteMarks[] = {
		"qq音乐", "网易云", "汽水音乐", "酷狗", "酷我", "spotify",
		"youtube", "bilibili", "音乐", "music"
	};

	std::string song = TrimCopy(a_title);
	std::string artist = TrimCopy(a_artist);

	// Cut trailing "… | 站点" / "… - 站点" fragments, at most twice.
	for (int pass = 0; pass < 2; ++pass) {
		const std::size_t cut = song.find_last_of("|");
		std::size_t sep = cut;
		const std::size_t dash = song.rfind(" - ");
		if (dash != std::string::npos && (sep == std::string::npos || dash > sep)) sep = dash;
		if (sep == std::string::npos) break;

		std::string tail = Lower(song.substr(sep));
		bool isSite = false;
		for (const char* m : kSiteMarks) {
			if (tail.find(m) != std::string::npos) { isSite = true; break; }
		}
		if (!isSite) break;
		song = TrimCopy(song.substr(0, sep));
	}

	if (artist.empty()) {
		// Never pass `song` as both input and output: SplitTitleArtist clears
		// its output first, which would erase the very text it must parse.
		const std::string source = song;
		std::string guessed;
		SplitTitleArtist(source, &song, &guessed);
		artist = guessed;
	}
	if (a_song) *a_song = TrimCopy(song);
	if (a_artistOut) *a_artistOut = artist;
}

void SplitTitleArtist(const std::string& a_title, std::string* a_song, std::string* a_artist)
{
	if (a_song) a_song->clear();
	if (a_artist) a_artist->clear();
	if (a_title.empty()) return;

	// Players put "song - artist" in the title bar.  Only split on the ASCII
	// hyphen surrounded by spaces: song names contain hyphens of their own
	// ("G-Eazy"), and a wrong split is worse than no artist at all.
	static const char* const kSep[] = { " - ", " – ", " — ", " | " };
	for (const char* sep : kSep) {
		const auto at = a_title.find(sep);
		if (at == std::string::npos || at == 0) continue;
		if (a_song) *a_song = a_title.substr(0, at);
		if (a_artist) *a_artist = a_title.substr(at + std::char_traits<char>::length(sep));
		return;
	}
	if (a_song) *a_song = a_title;
}

}  // namespace imr
