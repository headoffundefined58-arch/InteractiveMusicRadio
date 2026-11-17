// InteractiveMusicRadio - audio session controller (Windows core audio).
//
// The "media control centre" brain: enumerate every audio session on the
// machine (each running program that is producing sound), snapshot its volume /
// mute / activity, and let the player duck, mute or spotlight a source.  The
// external programs keep playing in their own process - IMR never injects into
// them and never pipes their audio through Skyrim.  What IMR *does* is observe
// them and, when one is selected, hold the local music layer down (via the CMS
// handshake) so the external source is what you actually hear.
//
// Every capture/restore is remembered so that on unload (game exit) all
// adjusted sessions are returned to their previous state - that is the
// "disconnect automatically on exit" guarantee.
#pragma once

#include <string>
#include <vector>

#include "core.h"

namespace imr {

class AudioSessionController {
public:
	AudioSessionController() = default;
	~AudioSessionController();

	AudioSessionController(const AudioSessionController&) = delete;
	AudioSessionController& operator=(const AudioSessionController&) = delete;

	// Initialise COM (MTA) and locate the default render endpoint.  False means
	// the machine has no default audio device / COM is unavailable; the plugin
	// degrades gracefully (no session control, still loads).
	bool Init(std::string* a_err);

	// Re-enumerate sessions and update the snapshot in place.  Cheap enough to
	// call at iSessionPollMs.  Returns true when anything changed.
	bool Poll(std::vector<AudioSession>& a_out);

	// Apply a volume (0..1) and mute to one session, remembering its previous
	// values so Restore() can undo it.
	bool SetVolume(const std::string& a_id, float a_volume, bool a_muted);

	// Same, addressed by owning process: the radio talks in sources, not in
	// session identifiers, and a restarted player keeps its pid but not its id.
	bool SetVolumeForPid(std::uint64_t a_pid, float a_volume, bool a_muted);

	// Restore every session we touched, then release the endpoint.
	void RestoreAll();

	// True while a device is attached and COM is up.
	bool Ready() const { return ready_; }

private:
	struct Impl;
	Impl* impl_ = nullptr;
	bool ready_ = false;
};

}  // namespace imr
