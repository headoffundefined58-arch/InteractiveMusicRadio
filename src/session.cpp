// InteractiveMusicRadio - audio session controller implementation.
#include "session.h"

#include "sources.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace imr {

namespace {
// What we did to a session, so RestoreAll() can put it back exactly.
struct Capture {
	float    prevVolume = 1.0f;
	bool     prevMuted = false;
	bool     touched = false;
};
}

struct AudioSessionController::Impl {
	IMMDeviceEnumerator* enumerator = nullptr;
	IMMDevice*           device = nullptr;
	IAudioSessionManager2* manager = nullptr;
	std::mutex            mtx;
	std::unordered_map<std::string, Capture> captured;
};

AudioSessionController::~AudioSessionController()
{
	RestoreAll();
	if (impl_) {
		if (impl_->manager) impl_->manager->Release();
		if (impl_->device) impl_->device->Release();
		if (impl_->enumerator) impl_->enumerator->Release();
		delete impl_;
		impl_ = nullptr;
	}
}

bool AudioSessionController::Init(std::string* a_err)
{
	// MTA is the correct apartment for Core Audio; if COM is already up in STA
	// (it is in the Skyrim process) CoInitializeEx returns RPC_E_CHANGED_MODE,
	// which we treat as "already initialised".
	const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool coOk = SUCCEEDED(hrCo) || hrCo == RPC_E_CHANGED_MODE;

	impl_ = new Impl();
	if (!coOk || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
			CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			reinterpret_cast<void**>(&impl_->enumerator)))) {
		if (a_err) *a_err = "CoCreateInstance(MMDeviceEnumerator) failed";
		return false;
	}

	if (FAILED(impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
			&impl_->device)) || !impl_->device) {
		if (a_err) *a_err = "no default render endpoint";
		return false;
	}

	if (FAILED(impl_->device->Activate(__uuidof(IAudioSessionManager2),
			CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&impl_->manager))) ||
		!impl_->manager) {
		if (a_err) *a_err = "Activate(IAudioSessionManager2) failed";
		return false;
	}

	ready_ = true;
	return true;
}

bool AudioSessionController::Poll(std::vector<AudioSession>& a_out)
{
	if (!ready_ || !impl_ || !impl_->manager) return false;

	std::vector<AudioSession> found;

	IAudioSessionEnumerator* enumerator = nullptr;
	if (SUCCEEDED(impl_->manager->GetSessionEnumerator(&enumerator)) && enumerator) {
		int count = 0;
		enumerator->GetCount(&count);
		for (int i = 0; i < count; ++i) {
			IAudioSessionControl* ctl = nullptr;
			if (FAILED(enumerator->GetSession(i, &ctl)) || !ctl) continue;

			AudioSession s;

			// process id
			IAudioSessionControl2* ctl2 = nullptr;
			if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
					reinterpret_cast<void**>(&ctl2))) && ctl2) {
				DWORD pid = 0;
				ctl2->GetProcessId(&pid);
				s.pid = pid;

				// stable session id
				LPWSTR sid = nullptr;
				if (SUCCEEDED(ctl2->GetSessionIdentifier(&sid)) && sid) {
					char utf8[512] = {};
					WideCharToMultiByte(CP_UTF8, 0, sid, -1, utf8,
						static_cast<int>(sizeof(utf8)) - 1, nullptr, nullptr);
					s.id = utf8;
					s.exe = ExeBaseName(utf8);
					CoTaskMemFree(sid);
				}

				// display name (fall back to process name)
				LPWSTR dn = nullptr;
				if (SUCCEEDED(ctl2->GetSessionInstanceIdentifier(&dn)) && dn) {
					CoTaskMemFree(dn);   // instance id is not the friendly name
				}
				if (SUCCEEDED(ctl2->GetDisplayName(&dn)) && dn && *dn) {
					char utf8[512] = {};
					WideCharToMultiByte(CP_UTF8, 0, dn, -1, utf8,
						static_cast<int>(sizeof(utf8)) - 1, nullptr, nullptr);
					s.name = utf8;
					CoTaskMemFree(dn);
				}
				ctl2->Release();
			}

			// Only the six catalogued programs ever reach RefreshSources():
			// AudioFor() matches on s.exe alone.  Dropping every other session
			// here skips the meter / volume / display-name COM round trips,
			// which is the bulk of this 2 Hz sweep, without changing a single
			// card the radio can show.
			if (!ClassifyApp(s.exe, "", "")) {
				ctl->Release();
				continue;
			}
			if (s.name.empty()) s.name = "process " + std::to_string(s.pid);

			// active state
			IAudioMeterInformation* meter = nullptr;
			if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioMeterInformation),
					reinterpret_cast<void**>(&meter))) && meter) {
				float peak = 0.0f;
				meter->GetPeakValue(&peak);
				s.active = peak > 0.0001f;
				meter->Release();
			}

			// volume / mute
			ISimpleAudioVolume* vol = nullptr;
			if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume),
					reinterpret_cast<void**>(&vol))) && vol) {
				vol->GetMasterVolume(&s.volume);
				BOOL muted = FALSE;
				vol->GetMute(&muted);
				s.muted = muted != FALSE;
				vol->Release();
			}

			// remembered capture state
			{
				std::lock_guard<std::mutex> lk(impl_->mtx);
				auto it = impl_->captured.find(s.id);
				s.captured = (it != impl_->captured.end() && it->second.touched);
			}

			if (!s.id.empty()) found.push_back(std::move(s));
			ctl->Release();
		}
		enumerator->Release();
	}

	const bool changed = (found.size() != a_out.size());
	a_out = std::move(found);
	return changed;
}

bool AudioSessionController::SetVolume(const std::string& a_id, float a_volume, bool a_muted)
{
	if (!ready_ || !impl_ || !impl_->manager || a_id.empty()) return false;

	IAudioSessionEnumerator* enumerator = nullptr;
	if (FAILED(impl_->manager->GetSessionEnumerator(&enumerator)) || !enumerator) return false;

	bool ok = false;
	int count = 0;
	enumerator->GetCount(&count);
	for (int i = 0; i < count && !ok; ++i) {
		IAudioSessionControl* ctl = nullptr;
		if (FAILED(enumerator->GetSession(i, &ctl)) || !ctl) continue;

		IAudioSessionControl2* ctl2 = nullptr;
		if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
				reinterpret_cast<void**>(&ctl2))) && ctl2) {
			LPWSTR sid = nullptr;
			std::string id;
			if (SUCCEEDED(ctl2->GetSessionIdentifier(&sid)) && sid) {
				char utf8[512] = {};
				WideCharToMultiByte(CP_UTF8, 0, sid, -1, utf8,
					static_cast<int>(sizeof(utf8)) - 1, nullptr, nullptr);
				id = utf8;
				CoTaskMemFree(sid);
			}
			ctl2->Release();

			if (id == a_id) {
				ISimpleAudioVolume* vol = nullptr;
				if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume),
						reinterpret_cast<void**>(&vol))) && vol) {
					// remember previous values once, before first write
					{
						std::lock_guard<std::mutex> lk(impl_->mtx);
						auto& cap = impl_->captured[a_id];
						if (!cap.touched) {
							vol->GetMasterVolume(&cap.prevVolume);
							BOOL muted = FALSE;
							vol->GetMute(&muted);
							cap.prevMuted = muted != FALSE;
							cap.touched = true;
						}
					}
					vol->SetMasterVolume(a_volume, nullptr);
					vol->SetMute(a_muted ? TRUE : FALSE, nullptr);
					vol->Release();
					ok = true;
				}
			}
		}
		ctl->Release();
	}
	enumerator->Release();
	return ok;
}

bool AudioSessionController::SetVolumeForPid(std::uint64_t a_pid, float a_volume,
    bool a_muted)
{
	if (!ready_ || !impl_ || !impl_->manager || a_pid == 0) return false;

	IAudioSessionEnumerator* enumerator = nullptr;
	if (FAILED(impl_->manager->GetSessionEnumerator(&enumerator)) || !enumerator) return false;

	bool ok = false;
	int count = 0;
	enumerator->GetCount(&count);
	for (int i = 0; i < count && !ok; ++i) {
		IAudioSessionControl* ctl = nullptr;
		if (FAILED(enumerator->GetSession(i, &ctl)) || !ctl) continue;

		IAudioSessionControl2* ctl2 = nullptr;
		if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
				reinterpret_cast<void**>(&ctl2))) && ctl2) {
			DWORD pid = 0;
			ctl2->GetProcessId(&pid);

			if (pid == static_cast<DWORD>(a_pid)) {
				LPWSTR sid = nullptr;
				std::string id;
				if (SUCCEEDED(ctl2->GetSessionIdentifier(&sid)) && sid) {
					char utf8[512] = {};
					WideCharToMultiByte(CP_UTF8, 0, sid, -1, utf8,
						static_cast<int>(sizeof(utf8)) - 1, nullptr, nullptr);
					id = utf8;
					CoTaskMemFree(sid);
				}

				ISimpleAudioVolume* vol = nullptr;
				if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume),
						reinterpret_cast<void**>(&vol))) && vol) {
					if (!id.empty()) {
						std::lock_guard<std::mutex> lk(impl_->mtx);
						auto& cap = impl_->captured[id];
						if (!cap.touched) {
							vol->GetMasterVolume(&cap.prevVolume);
							BOOL muted = FALSE;
							vol->GetMute(&muted);
							cap.prevMuted = muted != FALSE;
							cap.touched = true;
						}
					}
					vol->SetMasterVolume(a_volume, nullptr);
					vol->SetMute(a_muted ? TRUE : FALSE, nullptr);
					vol->Release();
					ok = true;
				}
			}
			ctl2->Release();
		}
		ctl->Release();
	}
	enumerator->Release();
	return ok;
}

void AudioSessionController::RestoreAll()
{
	if (!impl_) return;
	// Restore every session we touched.  Best-effort: sessions that have since
	// closed simply fail to match and are skipped.
	{
		std::lock_guard<std::mutex> lk(impl_->mtx);
		if (impl_->captured.empty()) return;
	}

	if (impl_->manager) {
		IAudioSessionEnumerator* enumerator = nullptr;
		if (SUCCEEDED(impl_->manager->GetSessionEnumerator(&enumerator)) && enumerator) {
			int count = 0;
			enumerator->GetCount(&count);
			for (int i = 0; i < count; ++i) {
				IAudioSessionControl* ctl = nullptr;
				if (FAILED(enumerator->GetSession(i, &ctl)) || !ctl) continue;
				IAudioSessionControl2* ctl2 = nullptr;
				if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
						reinterpret_cast<void**>(&ctl2))) && ctl2) {
					LPWSTR sid = nullptr;
					std::string id;
					if (SUCCEEDED(ctl2->GetSessionIdentifier(&sid)) && sid) {
						char utf8[512] = {};
						WideCharToMultiByte(CP_UTF8, 0, sid, -1, utf8,
							static_cast<int>(sizeof(utf8)) - 1, nullptr, nullptr);
						id = utf8;
						CoTaskMemFree(sid);
					}
					ctl2->Release();

					Capture cap{};
					bool have = false;
					{
						std::lock_guard<std::mutex> lk(impl_->mtx);
						auto it = impl_->captured.find(id);
						if (it != impl_->captured.end() && it->second.touched) {
							cap = it->second;
							have = true;
						}
					}
					if (have) {
						ISimpleAudioVolume* vol = nullptr;
						if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume),
								reinterpret_cast<void**>(&vol))) && vol) {
							vol->SetMasterVolume(cap.prevVolume, nullptr);
							vol->SetMute(cap.prevMuted ? TRUE : FALSE, nullptr);
							vol->Release();
						}
					}
				}
				ctl->Release();
			}
			enumerator->Release();
		}
	}

	std::lock_guard<std::mutex> lk(impl_->mtx);
	impl_->captured.clear();
}

}  // namespace imr
