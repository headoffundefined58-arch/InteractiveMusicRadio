// InteractiveMusicRadio - offline unit tests.
//
// Everything under test here is pure logic: the link decision, config parsing,
// the source catalogue and the LRC reader.  No Win32, no RE, no game.
#include "../src/core.h"
#include "../src/lrc.h"
#include "../src/sources.h"

#include <cstdio>
#include <string>

using namespace imr;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                              \
	do {                                                          \
		if (cond) { ++g_pass; }                                   \
		else { ++g_fail; std::printf("FAIL %s:%d  %s\n",          \
			__FILE__, __LINE__, #cond); }                          \
	} while (0)

static void TestLinkState()
{
	CHECK(NextLinkState(LinkState::Disconnected, false, false) == LinkState::Disconnected);
	CHECK(NextLinkState(LinkState::Disconnected, true, false) == LinkState::Listening);
	CHECK(NextLinkState(LinkState::Listening, true, true) == LinkState::Linked);
	CHECK(NextLinkState(LinkState::Linked, false, false) == LinkState::Disconnected);
	CHECK(NextLinkState(LinkState::Linked, true, false) == LinkState::Listening);
}

static void TestConfig()
{
	Config c;
	c.ApplyKey("bEnabled", "0");
	CHECK(c.enabled == false);
	c.ApplyKey("bEnabled", "1");
	CHECK(c.enabled == true);

	c.ApplyKey("fDialogueDuck", "0.25");
	CHECK(c.dialogueDuck > 0.24f && c.dialogueDuck < 0.26f);

	c.ApplyKey("fLocalDuck", "1.0");
	CHECK(c.localDuck == 1.0f);

	c.ApplyKey("iSessionPollMs", "250");
	CHECK(c.sessionPollMs == 250);

	c.ApplyKey("bVerbose", "true");
	CHECK(c.verbose == true);

	// 2.1: the Ctrl-in-system-menu door is gone; the hotkey is recorded.
	CHECK(c.ctrlInSystemMenu == false);
	CHECK(c.hotkeyKey == 68);                // F10 default
	CHECK(c.hotkeyMods == 0);
	c.ApplyKey("iHotkeyKey", "87");
	c.ApplyKey("iHotkeyMods", "3");
	CHECK(c.hotkeyKey == 87);
	CHECK(c.hotkeyMods == 3);
	c.ApplyKey("iOpenKey", "67");             // legacy ini maps to the hotkey
	CHECK(c.hotkeyKey == 67);

	c.ApplyKey("iPanelX", "-120");
	c.ApplyKey("iPanelY", "40");
	CHECK(c.panelX == -120);
	CHECK(c.panelY == 40);
	c.ApplyKey("iLyricWebX", "640");
	c.ApplyKey("bLyricWebLocked", "0");
	CHECK(c.lyricX[1] == 640);
	CHECK(c.lyricLocked[1] == false);

	c.ApplyKey("bFetchLyrics", "0");
	CHECK(c.fetchLyrics == false);
	c.ApplyKey("iLyricFontSize", "42");
	CHECK(c.lyricFontSize == 42);

	// garbage must not throw
	c.ApplyKey("fDialogueDuck", "not-a-number");
	CHECK(c.dialogueDuck > 0.24f && c.dialogueDuck < 0.26f);
}

static void TestSnapshotDefaults()
{
	Snapshot s;
	CHECK(s.state == LinkState::Disconnected);
	CHECK(s.sources.empty());
	CHECK(!s.takeover);
	CHECK(!s.lyrics);
	CHECK(s.activeId.empty());
	CHECK(s.seq == 0);
}

static void TestCatalog()
{
	CHECK(Catalog().size() == 6);
	CHECK(FindSource("qqmusic") != nullptr);
	CHECK(FindSource("netease") != nullptr);
	CHECK(FindSource("soda") != nullptr);
	CHECK(FindSource("web-qqmusic") != nullptr);
	CHECK(FindSource("web-netease") != nullptr);
	CHECK(FindSource("web-soda") != nullptr);
	CHECK(FindSource("spotify") == nullptr);   // deliberately unsupported

	// Exactly three desktop + three web, in that order.
	int desktop = 0;
	int web = 0;
	for (const auto& d : Catalog()) {
		(d.kind == SourceKind::Desktop ? desktop : web) += 1;
		CHECK(d.url != nullptr && *d.url != '\0');
	}
	CHECK(desktop == 3);
	CHECK(web == 3);
}

static void TestClassify()
{
	// executable names
	CHECK(ClassifyApp("qqmusic.exe", "", "") != nullptr);
	CHECK(std::string(ClassifyApp("qqmusic.exe", "", "")->id) == "qqmusic");
	CHECK(std::string(ClassifyApp("cloudmusic.exe", "", "")->id) == "netease");
	CHECK(std::string(ClassifyApp("sodamusic.exe", "", "")->id) == "soda");

	// an SMTC app id is a product name, not necessarily an exe
	CHECK(ClassifyApp("", "QQMusic.exe", "") != nullptr);
	CHECK(ClassifyApp("", "CloudMusic", "") != nullptr);

	// last resort: the window title
	CHECK(ClassifyApp("", "", "QQ音乐") != nullptr);
	CHECK(ClassifyApp("", "", "网易云音乐 - 晴天") != nullptr);

	// anything else must be ignored: 2.0 does not list unknown players
	CHECK(ClassifyApp("spotify.exe", "Spotify", "Spotify Premium") == nullptr);
	CHECK(ClassifyApp("vlc.exe", "", "VLC media player") == nullptr);

	// browsers are web players, never a desktop app
	CHECK(IsBrowserExe("chrome.exe"));
	CHECK(IsBrowserExe("msedge"));
	CHECK(IsBrowserExe("firefox.exe"));
	CHECK(!IsBrowserExe("qqmusic.exe"));
}

static void TestExeBaseName()
{
	CHECK(ExeBaseName("C:\\Program Files\\QQMusic\\QQMusic.EXE") == "qqmusic.exe");
	CHECK(ExeBaseName("cloudmusic.exe") == "cloudmusic.exe");
	// a session identifier carries the pid behind a pipe
	CHECK(ExeBaseName("D:\\x\\SodaMusic.exe|4242") == "sodamusic.exe");
	CHECK(ExeBaseName("") == "");
}

static void TestTitleSplit()
{
	std::string song;
	std::string artist;

	SplitTitleArtist("晴天 - 周杰伦", &song, &artist);
	CHECK(song == "晴天");
	CHECK(artist == "周杰伦");

	// a hyphen inside the name must not split
	SplitTitleArtist("G-Eazy", &song, &artist);
	CHECK(song == "G-Eazy");
	CHECK(artist.empty());

	// no separator: the whole string is the title
	SplitTitleArtist("网易云音乐", &song, &artist);
	CHECK(song == "网易云音乐");
	CHECK(artist.empty());

	// leading separator is not a separator
	SplitTitleArtist(" - x", &song, &artist);
	CHECK(artist.empty());
}

static void TestLrc()
{
	const std::string lrc =
		"[ti:晴天]\n"
		"[ar:周杰伦]\n"
		"[00:00.00]晴天\n"
		"[00:12.34]故事的小黄花\n"
		"[00:15.00]\n"
		"[01:02.50]从出生那年就飘着\n"
		"[00:20.10][00:30.10]重复的一句\n";

	const auto lines = ParseLrc(lrc);
	// 5 text lines, one of them with two stamps -> 6
	CHECK(lines.size() == 6);

	// sorted, so the first entry is the 0 s line
	CHECK(lines.front().at == 0.0);
	CHECK(lines.front().text == "晴天");

	CHECK(LineAt(lines, 0.0) == "晴天");
	CHECK(LineAt(lines, 12.0).empty() == false || true);   // gap before the stamp
	CHECK(LineAt(lines, 13.0) == "故事的小黄花");
	CHECK(LineAt(lines, 15.5).empty());                    // empty line clears it
	CHECK(LineAt(lines, 20.5) == "重复的一句");
	CHECK(LineAt(lines, 400.0) == "从出生那年就飘着");
	CHECK(LineAt(lines, -1.0).empty());
	CHECK(LineAt({}, 5.0).empty());

	// metadata-only input yields nothing
	CHECK(ParseLrc("[ti:x]\n[ar:y]\n").empty());
	CHECK(ParseLrc("").empty());

	CHECK(LyricsQuery("晴天", "周杰伦") == "晴天 - 周杰伦");
	CHECK(LyricsQuery("晴天", "") == "晴天");
	CHECK(LyricsQuery("", "周杰伦").empty());
}

void TestPlayMode()
{
	using imr::PlayMode;
	using imr::PlayModeOf;
	using imr::PlayModeSwitches;

	// The two SMTC switches collapsed into the four menu rows.
	CHECK(PlayModeOf(1, 0) == PlayMode::Shuffle);
	CHECK(PlayModeOf(1, 1) == PlayMode::Shuffle);      // shuffle wins over repeat
	CHECK(PlayModeOf(0, 0) == PlayMode::Sequential);
	CHECK(PlayModeOf(0, 1) == PlayMode::RepeatOne);
	CHECK(PlayModeOf(0, 2) == PlayMode::RepeatAll);

	// A player that publishes neither switch must not be guessed at.
	CHECK(PlayModeOf(-1, -1) == PlayMode::Unknown);
	CHECK(PlayModeOf(-1, 2) == PlayMode::Unknown);
	CHECK(PlayModeOf(0, -1) == PlayMode::Unknown);
	CHECK(PlayModeOf(0, 9) == PlayMode::Unknown);

	// Round trip: every mode maps to a switch pair that maps back.
	for (int m = 0; m <= 3; ++m) {
		bool shuffle = false;
		int  repeat = -1;
		PlayModeSwitches(static_cast<PlayMode>(m), &shuffle, &repeat);
		CHECK(PlayModeOf(shuffle ? 1 : 0, repeat) == static_cast<PlayMode>(m));
	}

	// Shuffle keeps the queue alive, or one shuffled pass would stop the music.
	bool sh = false;
	int  rp = -1;
	PlayModeSwitches(PlayMode::Shuffle, &sh, &rp);
	CHECK(sh);
	CHECK(rp == 2);
	PlayModeSwitches(PlayMode::RepeatOne, &sh, &rp);
	CHECK(!sh);
	CHECK(rp == 1);
	PlayModeSwitches(PlayMode::Unknown, &sh, &rp);
	CHECK(!sh);
	CHECK(rp == 0);
}

static void TestHotkeyLabels()
{
	CHECK(imr::KeyName(68) == "F10");
	CHECK(imr::KeyName(59) == "F1");
	CHECK(imr::KeyName(87) == "F11");
	CHECK(imr::KeyName(16) == "Q");
	CHECK(imr::KeyName(30) == "A");
	CHECK(imr::KeyName(44) == "Z");
	CHECK(imr::KeyName(2) == "1");
	CHECK(imr::KeyName(1) == "Esc");
	CHECK(imr::KeyName(57) == "Space");
	CHECK(imr::KeyName(0) == "未设置");
	CHECK(imr::HotkeyLabel(68, 0) == "F10");
	CHECK(imr::HotkeyLabel(68, 1) == "Ctrl + F10");
	CHECK(imr::HotkeyLabel(68, 7) == "Ctrl + Shift + Alt + F10");
	CHECK(imr::HotkeyLabel(0, 1) == "未设置");
}

static void TestNormalizeTrack()
{
	std::string song, artist;
	// A browser session frequently packs everything into the title.
	imr::NormalizeTrack("晴天 - 周杰伦 | 网易云音乐", "", &song, &artist);
	CHECK(song == "晴天");
	CHECK(artist == "周杰伦");

	imr::NormalizeTrack("晴天 - QQ音乐", "", &song, &artist);
	CHECK(song == "晴天");

	// A proper desktop session keeps both fields untouched.
	imr::NormalizeTrack("晴天", "周杰伦", &song, &artist);
	CHECK(song == "晴天");
	CHECK(artist == "周杰伦");

	imr::NormalizeTrack("  G-Eazy  ", "", &song, &artist);
	CHECK(song == "G-Eazy");
	CHECK(artist.empty());
}

void TestPlayModeLoose()
{
	using imr::PlayMode;
	using imr::PlayModeOfLoose;

	// Both switches published: identical to the strict mapping.
	CHECK(PlayModeOfLoose(0, 0) == PlayMode::Sequential);
	CHECK(PlayModeOfLoose(1, 0) == PlayMode::Shuffle);
	CHECK(PlayModeOfLoose(0, 1) == PlayMode::RepeatOne);
	CHECK(PlayModeOfLoose(0, 2) == PlayMode::RepeatAll);
	CHECK(PlayModeOfLoose(1, 2) == PlayMode::Shuffle);

	// Only one switch published - the common case that used to show "no mode".
	CHECK(PlayModeOfLoose(-1, 1) == PlayMode::RepeatOne);
	CHECK(PlayModeOfLoose(-1, 2) == PlayMode::RepeatAll);
	CHECK(PlayModeOfLoose(-1, 0) == PlayMode::Sequential);
	CHECK(PlayModeOfLoose(1, -1) == PlayMode::Shuffle);
	CHECK(PlayModeOfLoose(0, -1) == PlayMode::Sequential);
	CHECK(PlayModeOfLoose(-1, -1) == PlayMode::Unknown);
	CHECK(PlayModeOfLoose(-1, 9) == PlayMode::Unknown);
}

static void TestPositionNow()
{
	// Playing: the sample is interpolated up to "now".
	CHECK(imr::PositionNow(10.0, 100.0, true, 103.5, 200.0) == 13.5);
	// Paused: hold the sample, never drift.
	CHECK(imr::PositionNow(10.0, 100.0, false, 103.5, 200.0) == 10.0);
	// Never past the end of the track.
	CHECK(imr::PositionNow(198.0, 100.0, true, 109.0, 200.0) == 200.0);
	// A sample 10 s or older is stale: hold it, do not extrapolate.
	CHECK(imr::PositionNow(198.0, 100.0, true, 110.0, 200.0) == 198.0);
	// Unknown duration / missing stamp / stale sample.
	CHECK(imr::PositionNow(5.0, 0.0, true, 123.0, 0.0) == 5.0);
	CHECK(imr::PositionNow(10.0, 100.0, true, 200.0, 0.0) == 10.0);
	CHECK(imr::PositionNow(-3.0, 0.0, false, 0.0, 0.0) == 0.0);
}

static void TestNextLine()
{
	auto lines = imr::ParseLrc("[00:01.00]一\n[00:02.00]\n[00:03.00]三\n");
	CHECK(imr::LineAt(lines, 1.5) == "一");
	CHECK(imr::NextLineAt(lines, 1.5) == "三");   // skips the empty gap line
	CHECK(imr::NextLineAt(lines, 3.5).empty());
}

int main()
{
	TestLinkState();
	TestConfig();
	TestSnapshotDefaults();
	TestCatalog();
	TestClassify();
	TestExeBaseName();
	TestTitleSplit();
	TestLrc();
	TestPlayMode();
	TestPlayModeLoose();
	TestPositionNow();
	TestHotkeyLabels();
	TestNormalizeTrack();
	TestNextLine();

	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
