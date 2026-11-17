// InteractiveMusicRadio - now-playing via the Windows media session (SMTC).
#include "nowplaying.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// C++/WinRT ships with the Windows 10/11 SDK (Include\<ver>\cppwinrt), so this
// adds no package and no build step - only headers.
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>   // IVectorView iteration
#include <winrt/Windows.Media.h>            // MediaPlaybackAutoRepeatMode
#include <winrt/Windows.Media.Control.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace imr {

namespace {

namespace WMC = winrt::Windows::Media::Control;
using WMC::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

double Seconds(winrt::Windows::Foundation::TimeSpan a_ts)
{
	// winrt::TimeSpan *is* std::chrono::duration<int64_t, 100 ns>, so it is a
	// plain tick count; dividing directly keeps MSVC from reporting C7624 on a
	// nested duration alias.
	return static_cast<double>(a_ts.count()) / 10000000.0;
}

std::string Utf8(const winrt::hstring& a_s)
{
	return winrt::to_string(a_s);
}

// Every SMTC call is an IAsyncOperation.  An unbounded .get() on a hung player
// would freeze the engine loop - and with it the lyrics, the handshake and the
// UI.  A finite wait keeps the radio responsive; a timeout is reported exactly
// like the "refused by the player" result the synchronous call used to give.
template <typename A>
auto WaitResults(A& a_async, std::chrono::milliseconds a_limit)
	-> std::optional<decltype(a_async.GetResults())>
{
	try {
		if (a_async.wait_for(a_limit) !=
			winrt::Windows::Foundation::AsyncStatus::Completed) {
			return std::nullopt;
		}
		return a_async.GetResults();
	} catch (...) {
		return std::nullopt;
	}
}

template <typename A>
bool WaitOk(A& a_async, std::chrono::milliseconds a_limit)
{
	const auto r = WaitResults(a_async, a_limit);
	return r.has_value() && *r;
}

constexpr auto kMetaTimeout = std::chrono::milliseconds(60);
constexpr auto kCommandTimeout = std::chrono::milliseconds(400);

// SMTC reports a product name ("QQMusic.exe", "Chrome"); the plugin classifies
// on the executable-ish part, so strip any path the session may carry.
std::string AumidExe(const std::string& a_aumid);

WMC::GlobalSystemMediaTransportControlsSession FindSession(
	const std::vector<WMC::GlobalSystemMediaTransportControlsSession>& a_list,
	const std::string& a_aumid)
{
	const std::string want = AumidExe(a_aumid);
	WMC::GlobalSystemMediaTransportControlsSession best{ nullptr };
	for (const auto& s : a_list) {
		if (AumidExe(Utf8(s.SourceAppUserModelId())) != want) continue;
		try {
			if (s.GetPlaybackInfo().PlaybackStatus() ==
				GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
				return s;
			}
		} catch (...) {
		}
		if (!best) best = s;
	}
	return best;
}

std::string AumidExe(const std::string& a_aumid)
{
	std::string s = a_aumid;
	const auto cut = s.find_last_of("\\/");
	if (cut != std::string::npos) s.erase(0, cut + 1);
	for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

}  // namespace

struct NowPlaying::Impl {
	WMC::GlobalSystemMediaTransportControlsSessionManager mgr{ nullptr };
	std::vector<WMC::GlobalSystemMediaTransportControlsSession> sessions;
	std::string playing;
};

NowPlaying::~NowPlaying()
{
	if (impl_) {
		impl_->sessions.clear();
		impl_->mgr = nullptr;
		delete impl_;
		impl_ = nullptr;
	}
}

bool NowPlaying::Init(std::string* a_err)
{
	if (ready_) return true;
	if (tried_) return false;
	tried_ = true;

	// The engine thread is where Poll() runs, so WinRT is initialised there.
	// A thread already in an STA (RPC_E_CHANGED_MODE) still gets working
	// proxies, so that is a warning, not a failure.
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	} catch (winrt::hresult_error const& e) {
		IMR_LOG_W("nowplaying: init_apartment failed (0x%08X), continuing",
			static_cast<unsigned>(e.code().value));
	} catch (...) {
		IMR_LOG_W("nowplaying: init_apartment failed, continuing");
	}

	try {
		impl_ = new Impl();
		impl_->mgr = WMC::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
		if (!impl_->mgr) {
			if (a_err) *a_err = "RequestAsync returned null";
			return false;
		}
		ready_ = true;
		IMR_LOG_I("media session manager ready (SMTC)");
		return true;
	} catch (winrt::hresult_error const& e) {
		if (a_err) {
			char buf[160];
			std::snprintf(buf, sizeof(buf), "SMTC unavailable (0x%08X)",
				static_cast<unsigned>(e.code().value));
			*a_err = buf;
		}
		return false;
	} catch (...) {
		if (a_err) *a_err = "SMTC unavailable";
		return false;
	}
}

bool NowPlaying::Poll(std::vector<SessionInfo>& a_out)
{
	a_out.clear();
	if (!ready_ || !impl_ || !impl_->mgr) return false;

	std::string playing;
	try {
		impl_->sessions.clear();
		for (const auto& s : impl_->mgr.GetSessions()) {
			impl_->sessions.push_back(s);
		}

		for (const auto& s : impl_->sessions) {
			SessionInfo info;
			info.aumid = AumidExe(Utf8(s.SourceAppUserModelId()));

			try {
				auto propsOp = s.TryGetMediaPropertiesAsync();
				if (const auto props = WaitResults(propsOp, kMetaTimeout)) {
					info.title = Utf8(props->Title());
					info.artist = Utf8(props->Artist());
					info.album = Utf8(props->AlbumTitle());
				}
			} catch (...) {
				// A session can vanish between enumeration and the property
				// call; keep the entry so transport still works.
			}

			try {
				const auto playback = s.GetPlaybackInfo();
				info.playing =
					playback.PlaybackStatus() ==
					GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

				// Both switches are optional.  An app that never sets them
				// leaves the property empty, which stays -1 = "not published"
				// so the menu shows no tick instead of a wrong one.
				try {
					if (const auto sh = playback.IsShuffleActive()) {
						info.shuffle = sh.Value() ? 1 : 0;
					}
				} catch (...) {
				}
				try {
					if (const auto rp = playback.AutoRepeatMode()) {
						info.repeat = static_cast<int>(rp.Value());
					}
				} catch (...) {
				}

				const auto timeline = s.GetTimelineProperties();
				double pos = Seconds(timeline.Position());
				// Pos is the value the *app* last reported; LastUpdatedTime says
				// when.  Adding the elapsed time is what keeps lyrics on the
				// beat instead of up to a full poll interval behind.
				try {
					const auto age = winrt::clock::now() - timeline.LastUpdatedTime();
					const double extra = static_cast<double>(age.count()) / 10000000.0;
					if (info.playing && extra > 0.0 && extra < 30.0) pos += extra;
				} catch (...) {
				}
				info.position = pos;
				info.positionAt = NowSeconds();
				info.duration = Seconds(timeline.EndTime());
			} catch (...) {
			}

			if (info.playing && playing.empty()) playing = info.aumid;
			a_out.push_back(std::move(info));
		}
	} catch (winrt::hresult_error const& e) {
		IMR_LOG_W("nowplaying: poll failed (0x%08X)",
			static_cast<unsigned>(e.code().value));
		return false;
	} catch (...) {
		return false;
	}

	impl_->playing = playing;
	return true;
}

std::string NowPlaying::PlayingAumid()
{
	return impl_ ? impl_->playing : std::string();
}

bool NowPlaying::Command(const std::string& a_aumid, const char* a_act)
{
	if (!ready_ || !impl_ || a_aumid.empty() || !a_act) return false;

	const auto target = FindSession(impl_->sessions, a_aumid);
	if (!target) {
		IMR_LOG_W("nowplaying: no session for '%s'", a_aumid.c_str());
		return false;
	}

	try {
		const std::string act = a_act;
		if (act == "play")   { auto op = target.TryPlayAsync();            return WaitOk(op, kCommandTimeout); }
		if (act == "pause")  { auto op = target.TryPauseAsync();           return WaitOk(op, kCommandTimeout); }
		if (act == "toggle") { auto op = target.TryTogglePlayPauseAsync(); return WaitOk(op, kCommandTimeout); }
		if (act == "next")   { auto op = target.TrySkipNextAsync();        return WaitOk(op, kCommandTimeout); }
		if (act == "prev")   { auto op = target.TrySkipPreviousAsync();    return WaitOk(op, kCommandTimeout); }
		IMR_LOG_W("nowplaying: unknown transport action '%s'", act.c_str());
		return false;
	} catch (winrt::hresult_error const& e) {
		IMR_LOG_W("nowplaying: transport '%s' failed (0x%08X)", a_act,
			static_cast<unsigned>(e.code().value));
		return false;
	} catch (...) {
		return false;
	}
}

bool NowPlaying::SetPlayMode(const std::string& a_aumid, PlayMode a_mode)
{
	if (!ready_ || !impl_ || a_aumid.empty()) return false;
	if (a_mode == PlayMode::Unknown) return false;

	const auto target = FindSession(impl_->sessions, a_aumid);
	if (!target) {
		IMR_LOG_W("playmode: no session for '%s'", a_aumid.c_str());
		return false;
	}
	const std::string want = AumidExe(a_aumid);

	bool shuffle = false;
	int  repeat = 0;
	PlayModeSwitches(a_mode, &shuffle, &repeat);

	// Independent attempts: QQ音乐 answers both, some players only one, and a
	// single failure must not cancel the half that works.
	bool anyOk = false;
	bool shuffleOk = false;
	{
		auto op = target.TryChangeShuffleActiveAsync(shuffle);
		shuffleOk = WaitOk(op, kCommandTimeout);
		anyOk = anyOk || shuffleOk;
	}
	{
		const auto mode = repeat == 1 ? winrt::Windows::Media::MediaPlaybackAutoRepeatMode::Track
			: repeat == 2 ? winrt::Windows::Media::MediaPlaybackAutoRepeatMode::List
			: winrt::Windows::Media::MediaPlaybackAutoRepeatMode::None;
		auto op = target.TryChangeAutoRepeatModeAsync(mode);
		const bool repeatOk = WaitOk(op, kCommandTimeout);
		anyOk = anyOk || repeatOk;
		IMR_LOG_D("playmode: switch results shuffle=%d repeat=%d",
			shuffleOk ? 1 : 0, repeatOk ? 1 : 0);
	}

	IMR_LOG_I("playmode: %s -> shuffle=%d repeat=%d (%s)", want.c_str(),
		shuffle ? 1 : 0, repeat, anyOk ? "ok" : "refused by the player");
	return anyOk;
}

bool NowPlaying::Seek(const std::string& a_aumid, double a_seconds)
{
	if (!ready_ || !impl_ || a_aumid.empty() || a_seconds < 0.0) return false;
	const auto target = FindSession(impl_->sessions, a_aumid);
	if (!target) return false;

	try {
		const auto ticks = static_cast<std::int64_t>(a_seconds * 10000000.0);
		auto op = target.TryChangePlaybackPositionAsync(ticks);
		const bool ok = WaitOk(op, kCommandTimeout);
		IMR_LOG_I("seek: %.2fs -> %s", a_seconds, ok ? "ok" : "refused by the player");
		return ok;
	} catch (winrt::hresult_error const& e) {
		IMR_LOG_W("seek: failed (0x%08X)", static_cast<unsigned>(e.code().value));
		return false;
	} catch (...) {
		return false;
	}
}

}  // namespace imr
