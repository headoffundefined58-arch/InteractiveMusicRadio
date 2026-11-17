// InteractiveMusicRadio - the desktop lyric windows (our own overlays).
//
// A layered, always-on-top strip per source kind.  Locked (the default) the
// window is click-through; unlocked it takes the mouse and can be dragged, and
// a teal bar plus a hint line says so.  Created and pumped on the engine thread,
// so it never touches the game thread.
//
// Text goes into a 32-bit DIB with GDI, then alpha is derived from luminance and
// the buffer is premultiplied - that is what gives crisp antialiased text
// through UpdateLayeredWindow (a colour-keyed window would show black fringes).
#include "lyrics.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core.h"

namespace imr {

namespace {

std::wstring Widen(const std::string& a_utf8)
{
	if (a_utf8.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(),
		static_cast<int>(a_utf8.size()), nullptr, 0);
	if (n <= 0) return {};
	std::wstring out(static_cast<std::size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(), static_cast<int>(a_utf8.size()),
		out.data(), n);
	return out;
}

constexpr wchar_t kClass[] = L"IMR_DesktopLyrics";

// State that belongs to the Win32 window procedure.  Kept as a plain struct so
// WndProc (a free function) does not need access to the private nested Impl.
struct WindowState {
	bool      locked = true;
	bool      dragging = false;
	int       dragDx = 0;      // cursor offset inside the window at grab time
	int       dragDy = 0;
	int       posX = -1;       // -1 = automatic (bottom centre)
	int       posY = -1;
	std::function<void(int, int)> onMove;
};

LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_w, LPARAM a_l)
{
	auto* st = reinterpret_cast<WindowState*>(
		GetWindowLongPtrW(a_hwnd, GWLP_USERDATA));

	if (a_msg == WM_DESTROY) return 0;

	if (st && !st->locked) {
		switch (a_msg) {
		case WM_LBUTTONDOWN: {
			POINT p{};
			RECT  r{};
			GetCursorPos(&p);
			GetWindowRect(a_hwnd, &r);
			st->dragDx = p.x - r.left;
			st->dragDy = p.y - r.top;
			st->dragging = true;
			SetCapture(a_hwnd);
			return 0;
		}
		case WM_MOUSEMOVE:
			if (st->dragging) {
				POINT p{};
				GetCursorPos(&p);
				const int x = p.x - st->dragDx;
				const int y = p.y - st->dragDy;
				st->posX = x;
				st->posY = y;
				SetWindowPos(a_hwnd, nullptr, x, y, 0, 0,
					SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			}
			return 0;
		case WM_LBUTTONUP:
			if (st->dragging) {
				st->dragging = false;
				ReleaseCapture();
				if (st->onMove) st->onMove(st->posX, st->posY);
			}
			return 0;
		case WM_SETCURSOR:
			SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
			return TRUE;
		default:
			break;
		}
	}
	// Locked: no input handling at all, WS_EX_TRANSPARENT passes clicks through.
	return DefWindowProcW(a_hwnd, a_msg, a_w, a_l);
}

// One text line -> a premultiplied BGRA run on a black background.
void DrawLine(HDC a_dc, const std::wstring& a_text, int a_width, int a_top,
	int a_height, COLORREF a_color, bool a_bold)
{
	HFONT font = CreateFontW(
		-a_height, 0, 0, 0, a_bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
	HGDIOBJ old = SelectObject(a_dc, font);
	SetBkMode(a_dc, TRANSPARENT);
	SetTextColor(a_dc, a_color);

	RECT r{ 0, a_top, a_width, a_top + a_height };
	DrawTextW(a_dc, a_text.c_str(), static_cast<int>(a_text.size()), &r,
		DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

	SelectObject(a_dc, old);
	DeleteObject(font);
}

}  // namespace

struct LyricsOverlay::Impl {
	HWND                 hwnd = nullptr;
	HINSTANCE            instance = nullptr;
	int                  fontSize = 30;
	int                  kind = 0;
	std::string          title;
	std::string          sub;
	std::vector<LrcLine> lines;
	bool                 dirty = true;
	bool                 shown = false;
	double               pos = 0.0;
	WindowState          ws;
};

LyricsOverlay::~LyricsOverlay()
{
	Destroy();
}

bool LyricsOverlay::Create(const Config& a_cfg, int a_kind)
{
	if (impl_) return true;
	impl_ = new Impl();
	impl_->fontSize = a_cfg.lyricFontSize > 8 ? a_cfg.lyricFontSize : 30;
	impl_->kind = a_kind;
	impl_->ws.locked = a_cfg.lyricLocked[a_kind & 1];
	impl_->ws.posX = a_cfg.lyricX[a_kind & 1];
	impl_->ws.posY = a_cfg.lyricY[a_kind & 1];
	impl_->instance = GetModuleHandleW(nullptr);

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = &WndProc;
	wc.hInstance = impl_->instance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = kClass;
	// RegisterClassExW fails with ERROR_CLASS_ALREADY_EXISTS for the second
	// overlay (and on reload); the class carries no per-instance state.
	RegisterClassExW(&wc);

	LONG exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
	if (impl_->ws.locked) exStyle |= WS_EX_TRANSPARENT;

	impl_->hwnd = CreateWindowExW(
		static_cast<DWORD>(exStyle), kClass, L"IMR Lyrics", WS_POPUP,
		0, 0, 640, 120, nullptr, nullptr, impl_->instance, nullptr);

	if (!impl_->hwnd) {
		IMR_LOG_W("lyrics[%d]: CreateWindowEx failed (%lu)", a_kind, GetLastError());
		delete impl_;
		impl_ = nullptr;
		return false;
	}
	SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA,
		reinterpret_cast<LONG_PTR>(&impl_->ws));
	IMR_LOG_I("lyrics[%d]: window created (%s)", a_kind,
		impl_->ws.locked ? "locked" : "draggable");
	return true;
}

void LyricsOverlay::Destroy()
{
	if (!impl_) return;
	if (impl_->hwnd) {
		DestroyWindow(impl_->hwnd);
		impl_->hwnd = nullptr;
	}
	delete impl_;
	impl_ = nullptr;
	visible_ = false;
	current_.clear();
	next_.clear();
}

void LyricsOverlay::SetTrack(const std::string& a_title, const std::string& a_artist,
	const std::string& a_album)
{
	if (!impl_) return;
	std::string sub = a_artist;
	if (!a_album.empty()) {
		if (!sub.empty()) sub += " · ";
		sub += a_album;
	}
	impl_->title = a_title;
	impl_->sub = sub;
	impl_->lines.clear();
	impl_->dirty = true;
	current_.clear();
	next_.clear();
	// While a lyric is being looked up, the first line shows the track itself.
}

void LyricsOverlay::SetLyric(const std::vector<LrcLine>& a_lines)
{
	if (!impl_) return;
	impl_->lines = a_lines;
	impl_->dirty = true;
	current_.clear();
	next_.clear();
	Tick(impl_->pos);
}

void LyricsOverlay::Tick(double a_position)
{
	if (!impl_) return;
	impl_->pos = a_position;
	const std::string line = LineAt(impl_->lines, a_position);
	const std::string next = NextLineAt(impl_->lines, a_position);
	if (line == current_ && next == next_) return;
	current_ = line;
	next_ = next;
	impl_->dirty = true;
}

void LyricsOverlay::SetVisible(bool a_on)
{
	if (!impl_ || !impl_->hwnd) return;
	if (a_on == visible_) return;
	visible_ = a_on;
	if (a_on) {
		impl_->dirty = true;
	} else {
		ShowWindow(impl_->hwnd, SW_HIDE);
		impl_->shown = false;
	}
}

void LyricsOverlay::SetLocked(bool a_locked)
{
	locked_ = a_locked;
	if (!impl_ || !impl_->hwnd) return;
	impl_->ws.locked = a_locked;

	auto style = GetWindowLongPtrW(impl_->hwnd, GWL_EXSTYLE);
	if (a_locked) style |= WS_EX_TRANSPARENT;
	else style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
	SetWindowLongPtrW(impl_->hwnd, GWL_EXSTYLE, style);
	SetWindowPos(impl_->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	impl_->dirty = true;
	IMR_LOG_I("lyrics[%d]: %s", impl_->kind,
		a_locked ? "locked (click-through)" : "unlocked (drag to move)");
}

void LyricsOverlay::SetPosition(int a_x, int a_y)
{
	if (!impl_) return;
	impl_->ws.posX = a_x;
	impl_->ws.posY = a_y;
	impl_->dirty = true;
}

void LyricsOverlay::GetPosition(int* a_x, int* a_y) const
{
	if (a_x) *a_x = impl_ ? impl_->ws.posX : -1;
	if (a_y) *a_y = impl_ ? impl_->ws.posY : -1;
}

void LyricsOverlay::SetMoveCallback(std::function<void(int, int)> a_cb)
{
	if (impl_) impl_->ws.onMove = std::move(a_cb);
}

void LyricsOverlay::Pump()
{
	if (!impl_ || !impl_->hwnd) return;

	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (!visible_ || !impl_->dirty) return;
	impl_->dirty = false;

	// Two-line desktop-lyric look: the line that is singing, then the next one.
	// With no lyric yet, fall back to the track and its artist / album.
	const std::wstring line1 = Widen(current_.empty() ? impl_->title : current_);
	std::wstring line2;
	if (!impl_->ws.locked) {
		line2 = Widen(impl_->kind == 0
			? "拖动定位 · 桌面版歌词" : "拖动定位 · 网页版歌词");
	} else if (!next_.empty()) {
		line2 = Widen(next_);
	} else {
		line2 = Widen(current_.empty() ? impl_->sub : impl_->title);
	}
	if (line1.empty() && line2.empty()) return;

	MONITORINFO mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi))
		return;

	const int monW = mi.rcWork.right - mi.rcWork.left;
	const int monH = mi.rcWork.bottom - mi.rcWork.top;

	int fs = impl_->fontSize;
	if (fs < 12) fs = 12;
	const int small = fs > 18 ? static_cast<int>(fs * 0.55) : fs;

	const int width = monW * 7 / 10;
	const int height = static_cast<int>(fs * 1.7) + small + 24;

	int x = impl_->ws.posX;
	int y = impl_->ws.posY;
	if (x < 0 || y < 0) {
		x = mi.rcWork.left + (monW - width) / 2;
		// The two overlays get different default slots so both can be visible.
		y = mi.rcWork.bottom - height - 90 - impl_->kind * (height + 24);
	}
	const int minX = static_cast<int>(mi.rcWork.left);
	const int maxX = static_cast<int>(mi.rcWork.right) - width;
	const int minY = static_cast<int>(mi.rcWork.top);
	const int maxY = static_cast<int>(mi.rcWork.bottom) - height;
	x = std::clamp(x, minX, std::max(minX, maxX));
	y = std::clamp(y, minY, std::max(minY, maxY));

	HDC screen = GetDC(nullptr);
	if (!screen) return;

	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = width;
	bi.bmiHeader.biHeight = -height;   // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void* bits = nullptr;
	HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	HDC mem = CreateCompatibleDC(screen);

	if (dib && mem && bits) {
		HGDIOBJ oldBmp = SelectObject(mem, dib);
		std::memset(bits, 0, static_cast<std::size_t>(width) * height * 4);

		// Unlocked: a teal grip bar makes it obvious the window is draggable.
		if (!impl_->ws.locked) {
			HBRUSH brush = CreateSolidBrush(RGB(62, 230, 180));
			RECT grip{ 0, 0, width, 3 };
			FillRect(mem, &grip, brush);
			DeleteObject(brush);
		}

		DrawLine(mem, line1, width, 8, fs + 10, RGB(255, 255, 255), true);
		if (!line2.empty()) {
			DrawLine(mem, line2, width, 8 + fs + 12, small + 8,
				RGB(110, 231, 200), false);
		}

		// GDI left the alpha channel at 0: derive it from luminance and
		// premultiply, which is what UpdateLayeredWindow expects.
		auto* px = static_cast<std::uint32_t*>(bits);
		const std::size_t count = static_cast<std::size_t>(width) * height;
		for (std::size_t i = 0; i < count; ++i) {
			const std::uint32_t p = px[i];
			const std::uint32_t r = p & 0xFF;
			const std::uint32_t g = (p >> 8) & 0xFF;
			const std::uint32_t b = (p >> 16) & 0xFF;
			const std::uint32_t a = r > g ? (r > b ? r : b) : (g > b ? g : b);
			if (a == 0) {
				px[i] = 0;
			} else {
				const std::uint32_t pr = r * a / 255;
				const std::uint32_t pg = g * a / 255;
				const std::uint32_t pb = b * a / 255;
				px[i] = (a << 24) | (pb << 16) | (pg << 8) | pr;
			}
		}

		POINT src{ 0, 0 };
		POINT dst{ x, y };
		SIZE size{ width, height };
		BLENDFUNCTION blend{};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 235;
		blend.AlphaFormat = AC_SRC_ALPHA;

		UpdateLayeredWindow(impl_->hwnd, screen, &dst, &size, mem, &src, 0, &blend,
			ULW_ALPHA);
		if (!impl_->shown) {
			ShowWindow(impl_->hwnd, SW_SHOWNOACTIVATE);
			impl_->shown = true;
		}

		SelectObject(mem, oldBmp);
	}

	if (mem) DeleteDC(mem);
	if (dib) DeleteObject(dib);
	ReleaseDC(nullptr, screen);
}

}  // namespace imr
