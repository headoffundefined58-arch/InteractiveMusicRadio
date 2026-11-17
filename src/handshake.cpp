// InteractiveMusicRadio - IMR <-> CMS handshake implementation.
#include "handshake.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <string>

#include "core.h"   // IMR_LOG_*

namespace imr {

namespace {
constexpr wchar_t kName[] = L"IMR_CMS_HANDSHAKE";
constexpr wchar_t kMutexName[] = L"IMR_CMS_HANDSHAKE_MUTEX";
}  // namespace

CmsHandshake::~CmsHandshake()
{
	// Explicit release first: a reader that looks at the block between our
	// unmap and the process teardown must see 0, not a stale 1.
	Release();
	if (map_) {
		UnmapViewOfFile(map_);
		map_ = nullptr;
	}
	block_ = nullptr;
	owns_ = false;
}

bool CmsHandshake::Init(std::string* a_err)
{
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
		PAGE_READWRITE, 0, sizeof(ImrBlock), kName);
	if (!mapping) {
		if (a_err) *a_err = "CreateFileMappingW failed";
		return false;
	}
	owns_ = (GetLastError() != ERROR_ALREADY_EXISTS);

	map_ = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ImrBlock));
	CloseHandle(mapping);   // the view keeps the section alive
	if (!map_) {
		if (a_err) *a_err = "MapViewOfFile failed";
		return false;
	}
	block_ = static_cast<ImrBlock*>(map_);

	// If we created the section it is zero-filled; stamp it so a reader can tell
	// a real block from a page of garbage.  If it already existed (a restart),
	// leave the previous writer's state alone until our first Publish().
	if (owns_) {
		std::memset(block_, 0, sizeof(ImrBlock));
		block_->magic = kImrBlockMagic;
		block_->version = kImrBlockVersion;
		block_->pid = GetCurrentProcessId();
	}

	// Serialise writers with a named mutex (only IMR writes, but CMS's reader may
	// be in the middle of a copy when a future second writer appears).
	HANDLE mtx = CreateMutexW(nullptr, FALSE, kMutexName);
	if (mtx) CloseHandle(mtx);

	return true;
}

void CmsHandshake::Publish(std::uint32_t a_state)
{
	if (!block_) return;

	HANDLE mtx = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, kMutexName);
	if (!mtx) mtx = CreateMutexW(nullptr, FALSE, kMutexName);
	const bool locked = mtx && WaitForSingleObject(mtx, 50) == WAIT_OBJECT_0;

	block_->magic = kImrBlockMagic;
	block_->version = kImrBlockVersion;
	block_->state = a_state;
	block_->pid = GetCurrentProcessId();
	block_->flags = (a_state == 1u) ? 1u : 0u;   // bit 0 = an external source is audible
	// Written last, so a reader that sees a fresh heartbeat also sees the state
	// that belongs to it.
	block_->heartbeatMs = static_cast<std::uint64_t>(GetTickCount64());

	if (locked) {
		ReleaseMutex(mtx);
		CloseHandle(mtx);
	}
}

void CmsHandshake::SetTakeover(bool a_takeover)
{
	const bool changed = (a_takeover != takeover_);
	takeover_ = a_takeover;

	// Always publish: the heartbeat has to be refreshed on every tick or CMS
	// treats an active link as dead after kImrHeartbeatMaxMs.
	Publish(a_takeover ? 1u : 0u);

	if (changed) {
		IMR_LOG_I("handshake: %s", a_takeover ? "takeover published (CMS holds local music down)"
		                                      : "released (CMS owns the music again)");
	}
}

void CmsHandshake::Release()
{
	if (!block_) return;
	block_->state = 0;
	block_->flags = 0;
	block_->heartbeatMs = static_cast<std::uint64_t>(GetTickCount64());
	takeover_ = false;
}

}  // namespace imr
