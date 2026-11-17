// InteractiveMusicRadio - the source catalogue.
//
// 2.0 dropped "list every audio session": the radio only serves two kinds of
// source, and only these six of them.
//
//   desktop  QQ音乐 / 网易云音乐 / 汽水音乐   (the installed programs)
//   web      the same three services, played in the system browser
//
// Everything here is pure string logic so the unit tests can cover it without
// a game, a music app or a browser.
#pragma once

#include <string>
#include <vector>

#include "core.h"

namespace imr {

struct SourceDef {
	const char* id;
	SourceKind  kind;
	const char* url;         // where to open the web player when none is running
};

// The six entries, in display order (desktop first, then their web twins).
const std::vector<SourceDef>& Catalog();

// nullptr when the id is unknown.
const SourceDef* FindSource(const std::string& a_id);

// True for the executables we treat as "the system browser" (the web player
// runs inside one of them, so its media session is what we control).
bool IsBrowserExe(const std::string& a_exe);

// Map a running program to a source id.  Returns nullptr when it is not one of
// ours - that is the whole point of 2.0: unknown players are ignored, not
// listed.
//
//   a_exe    lower-case executable name, "qqmusic.exe"
//   a_aumid  media-session app id, "QQMusic.exe" / "Chrome"
//   a_title  window title, used as the last resort ("QQ音乐")
const SourceDef* ClassifyApp(const std::string& a_exe, const std::string& a_aumid,
    const std::string& a_title);

// Lower-case, path stripped: "C:\X\QQMusic.EXE" -> "qqmusic.exe".
std::string ExeBaseName(const std::string& a_path);

// "QQ音乐 - 晴天 - 周杰伦" style titles: split into title / artist when the
// separator is one the players actually use.  Anything else is kept verbatim
// as the title, so a bad guess never invents an artist.
void SplitTitleArtist(const std::string& a_title, std::string* a_song, std::string* a_artist);

// Browser media sessions report things like
//   "晴天 - 周杰伦 | 网易云音乐"   or   "晴天 - QQ音乐"
// while desktop apps fill both fields properly.  Clean that up before looking
// a lyric up, otherwise the query carries the site name and matches nothing.
void NormalizeTrack(const std::string& a_title, const std::string& a_artist,
	std::string* a_song, std::string* a_artistOut);

}  // namespace imr
