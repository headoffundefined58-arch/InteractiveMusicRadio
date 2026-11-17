// InteractiveMusicRadio - the IMR <-> CMS handshake.
//
// IMR never patches CMS and never links against it.  The two plugins meet in a
// single named shared section ("IMR_CMS_HANDSHAKE"), 64 bytes wide, laid out
// exactly like ImrBlock below - the same layout CMS declares as cms::ImrLink.
// IMR writes, CMS reads.
//
//   state == 1  the radio is on the air: hold the local music layer down, so
//               the linked external source is what the player hears
//   state == 0  idle / released: CMS owns the context again
//
// The heartbeat is what makes the link *safe*: a state word alone would stick at
// "takeover" forever if IMR crashed or was killed, leaving the game silent.  CMS
// ignores a block whose heartbeat is older than 3 s, so an abnormal exit drops
// the link by itself.  The clean exit path also zeroes the state explicitly.
//
// If CMS is not installed, or is older than the reader, nothing happens - IMR
// still works standalone (it just cannot duck the local layer).
//
// The block is torn down when IMR unloads: that is the "disconnect on exit"
// guarantee for the handshake side of the plugin.
#pragma once

#include <cstdint>
#include <string>

namespace imr {

// Binary contract with CMS.  Keep it fixed-width: the two plugins are separate
// DLLs built at different times and only agree on this byte layout.
struct ImrBlock {
	std::uint32_t magic = 0;         // 'IMRH'
	std::uint32_t version = 0;       // 1
	std::uint32_t state = 0;         // 0 idle, 1 takeover
	std::uint32_t pid = 0;
	std::uint64_t heartbeatMs = 0;
	std::uint32_t flags = 0;         // bit 0 = an external source is audible
	std::uint32_t pad[9] = {};
};

static_assert(sizeof(ImrBlock) == 64, "ImrBlock must match cms::ImrLink exactly");

constexpr std::uint32_t kImrBlockMagic = 0x48524D49u;   // 'IMRH'
constexpr std::uint32_t kImrBlockVersion = 1u;

class CmsHandshake {
public:
	CmsHandshake() = default;
	~CmsHandshake();

	CmsHandshake(const CmsHandshake&) = delete;
	CmsHandshake& operator=(const CmsHandshake&) = delete;

	// Create/attach the shared block.  Safe to call once at startup.
	bool Init(std::string* a_err);

	// Publish the takeover flag and refresh the heartbeat.  Call every engine
	// tick: the heartbeat is what keeps an active link alive on CMS's side.
	void SetTakeover(bool a_takeover);

	// Clear the state and stop the heartbeat (clean shutdown).
	void Release();

	// Whether we hold the block at all.
	bool Active() const { return block_ != nullptr; }

private:
	void Publish(std::uint32_t a_state);

	void*     map_ = nullptr;     // mapped view
	ImrBlock* block_ = nullptr;   // same page, typed
	bool      owns_ = false;      // we created the section
	bool      takeover_ = false;
};

}  // namespace imr
