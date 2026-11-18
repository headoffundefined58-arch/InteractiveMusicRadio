// InteractiveMusicRadio - video subtitle capture (WinRT implementation).
//
// Pipeline, all owned by one worker thread (its own MTA apartment):
//   browser HWND -> GraphicsCaptureItem -> free-threaded frame pool
//   -> newest frame -> D3D11 staging copy -> bottom-strip crop
//   -> SoftwareBitmap -> Windows OCR -> text stabiliser -> result slot
//
// The engine thread only calls Start/Stop/Take.  Take never blocks on WinRT:
// the worker publishes into a small mutex-protected slot.  Every stage is
// guarded, because a browser can close, minimise, or refuse capture at any
// moment; on any failure the worker clears the slot so the caller falls back
// to the network lyric.
#include "subtitle.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <inspectable.h>   // ABI IInspectable, used by the D3D11 device bridge
// The C capture-interop header must come before the C++/WinRT projection.
// We deliberately do NOT include windows.graphics.directx.direct3d11.interop.h:
// its COM declarations clash with the C++/WinRT projection.  Instead we use
// Kenny Kerr's minimal C++/WinRT-friendly shim: the documented C ABI function
// plus the one COM interface we need, both declared below.
#include <windows.graphics.capture.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sources.h"

namespace imr {
namespace {

namespace WGC = winrt::Windows::Graphics::Capture;
namespace WDX = winrt::Windows::Graphics::DirectX;
namespace WDX3D = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace WGI = winrt::Windows::Graphics::Imaging;
namespace WOCR = winrt::Windows::Media::Ocr;
namespace WGL = winrt::Windows::Globalization;
namespace WSS = winrt::Windows::Storage::Streams;

// Exported by d3d11.dll (see windows.graphics.directx.direct3d11.interop.h).
extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(
	::IDXGIDevice* a_dxgiDevice, ::IInspectable** a_graphicsDevice);

// C++/WinRT-friendly declaration of the DXGI interface accessor.  The SDK
// interop header declares this as a MIDL_INTERFACE that collides with the
// C++/WinRT projection; this plain COM declaration is ABI-identical and works
// with surface.as<>().  IID from windows.graphics.directx.direct3d11.interop.h.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
	IDirect3DDxgiInterfaceAccess : ::IUnknown
{
	virtual HRESULT WINAPI GetInterface(GUID const& a_id, void** a_object) = 0;
};

struct WindowPick {
	HWND   hwnd = nullptr;
	LONG   area = 0;
};

struct EnumCtx {
	std::string exeHintTitle;   // lower-cased track title
	WindowPick  best;
};

BOOL CALLBACK EnumBrowserWindow(HWND a_hwnd, LPARAM a_lparam)
{
	auto* ctx = reinterpret_cast<EnumCtx*>(a_lparam);
	if (!IsWindowVisible(a_hwnd)) return TRUE;
	if (IsIconic(a_hwnd)) return TRUE;   // WGC cannot capture a minimised window

	DWORD pid = 0;
	GetWindowThreadProcessId(a_hwnd, &pid);
	if (!pid) return TRUE;

	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc) return TRUE;
	wchar_t path[MAX_PATH] = {};
	DWORD len = MAX_PATH;
	const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &len);
	CloseHandle(proc);
	if (!ok) return TRUE;

	const std::string exe = ExeBaseName(winrt::to_string(std::wstring(path)));
	if (!IsBrowserExe(exe)) return TRUE;

	RECT r{};
	if (!GetWindowRect(a_hwnd, &r)) return TRUE;
	const LONG w = r.right - r.left;
	const LONG h = r.bottom - r.top;
	if (w < 320 || h < 240) return TRUE;

	LONG score = w * h;
	if (!ctx->exeHintTitle.empty()) {
		const int n = GetWindowTextLengthW(a_hwnd);
		if (n > 0) {
			std::wstring title(static_cast<std::size_t>(n) + 1, L'\0');
			GetWindowTextW(a_hwnd, title.data(), n + 1);
			std::string t = winrt::to_string(title);
			std::transform(t.begin(), t.end(), t.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (t.find(ctx->exeHintTitle) != std::string::npos) score += 1000000;
		}
	}
	if (score > ctx->best.area) ctx->best = { a_hwnd, score };
	return TRUE;
}

HWND FindBrowserWindow(const std::string& a_hint)
{
	EnumCtx ctx;
	ctx.exeHintTitle = a_hint;
	std::transform(ctx.exeHintTitle.begin(), ctx.exeHintTitle.end(),
		ctx.exeHintTitle.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	EnumWindows(&EnumBrowserWindow, reinterpret_cast<LPARAM>(&ctx));
	return ctx.best.hwnd;
}

// WGC does not expose an HWND-to-item projection; the C interop factory does.
WGC::GraphicsCaptureItem ItemFromWindow(HWND a_hwnd)
{
	auto factory = winrt::get_activation_factory<WGC::GraphicsCaptureItem,
		IGraphicsCaptureItemInterop>();
	WGC::GraphicsCaptureItem item{ nullptr };
	winrt::check_hresult(factory->CreateForWindow(
		a_hwnd, winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(item)));
	return item;
}

// D3D11 -> WinRT device bridge required by the frame pool.
WDX3D::IDirect3DDevice MakeWinrtDevice(ID3D11Device* a_device)
{
	winrt::com_ptr<IDXGIDevice> dxgi;
	winrt::check_hresult(a_device->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void()));
	winrt::com_ptr<winrt::Windows::Foundation::IInspectable> inspectable;
	winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
		dxgi.get(), reinterpret_cast<::IInspectable**>(inspectable.put())));
	return inspectable.as<WDX3D::IDirect3DDevice>();
}

}  // namespace

// --------------------------------------------------------------- Impl -------
struct VideoSubtitle::Impl {
	std::mutex          mtx;
	std::condition_variable cv;
	bool                stop = false;
	std::thread         worker;
	std::string         text;
	double              at = 0.0;

	VideoSubConfig      cfg;
	std::string         hint;
	HWND                target = nullptr;

	void Publish(std::string a_text)
	{
		std::lock_guard<std::mutex> lk(mtx);
		text = std::move(a_text);
		at = NowSeconds();
	}
	void Clear()
	{
		std::lock_guard<std::mutex> lk(mtx);
		text.clear();
		at = 0.0;
	}

	// Runs on the worker thread.  All WinRT objects live and die here.
	void Run()
	{
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		} catch (...) {
			IMR_LOG_W("videosub: init_apartment failed on worker");
		}

		bool apartment = true;
		try {
			WorkerBody();
		} catch (winrt::hresult_error const& e) {
			IMR_LOG_W("videosub: WinRT error 0x%08X - %s",
				static_cast<unsigned>(e.code().value),
				winrt::to_string(e.message()).c_str());
		} catch (...) {
			IMR_LOG_W("videosub: unexpected worker error");
		}
		(void)apartment;
		// uninit_apartment is intentionally skipped: the thread is about to
		// exit and calling it after WinRT teardown can crash on some builds.
	}

	void WorkerBody()
	{
		const HWND hwnd = target;
		if (!hwnd || !IsWindow(hwnd)) {
			IMR_LOG_W("videosub: no visible browser window to capture");
			return;
		}

		winrt::com_ptr<ID3D11Device> d3d;
		winrt::com_ptr<ID3D11DeviceContext> ctx;
		const HRESULT hr = D3D11CreateDevice(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
			d3d.put(), nullptr, ctx.put());
		if (FAILED(hr) || !d3d) {
			IMR_LOG_W("videosub: D3D11CreateDevice failed (0x%08X)", static_cast<unsigned>(hr));
			return;
		}

		WGC::GraphicsCaptureItem item = ItemFromWindow(hwnd);
		if (!item) {
			IMR_LOG_W("videosub: CreateForWindow failed");
			return;
		}

		auto framePool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
			MakeWinrtDevice(d3d.get()),
			WDX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
			2, item.Size());
		auto session = framePool.CreateCaptureSession(item);
		try { session.IsCursorCaptureEnabled(false); } catch (...) {}
		try { session.IsBorderRequired(false); } catch (...) {}
		session.StartCapture();
		const auto isz = item.Size();
		IMR_LOG_I("videosub: capturing browser window 0x%llX (%dx%d)",
			static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)),
			static_cast<int>(isz.Width), static_cast<int>(isz.Height));

		// Create the OCR engine once, on this thread, with a Chinese-first
		// preference so MV lyrics / CJK subtitles are recognised correctly.
		WOCR::OcrEngine engine{ nullptr };
		try {
			engine = WOCR::OcrEngine::TryCreateFromLanguage(WGL::Language(L"zh-Hans"));
			if (!engine) engine = WOCR::OcrEngine::TryCreateFromLanguage(WGL::Language(L"zh-CN"));
			if (!engine) engine = WOCR::OcrEngine::TryCreateFromUserProfileLanguages();
		} catch (...) {
		}
		if (!engine) {
			IMR_LOG_W("videosub: no usable OCR language pack");
			session.Close();
			framePool.Close();
			return;
		}

		SubtitleStabilizer stab(2);
		const auto interval = std::chrono::milliseconds(
			std::clamp(cfg.intervalMs, 120, 2000));

		for (;;) {
			{
				std::unique_lock<std::mutex> lk(mtx);
				if (cv.wait_for(lk, interval, [this] { return stop; })) break;
			}
			if (!IsWindow(hwnd)) {
				IMR_LOG_W("videosub: target window closed");
				break;
			}

			WGC::Direct3D11CaptureFrame frame{ nullptr };
			try { frame = framePool.TryGetNextFrame(); } catch (...) {}
			if (!frame) continue;

			std::string line;
			try {
				line = RecogniseFrame(frame, d3d.get(), ctx.get(), engine, cfg);
			} catch (winrt::hresult_error const& e) {
				IMR_LOG_D("videosub: OCR frame error 0x%08X", static_cast<unsigned>(e.code().value));
				stab.Miss();
				continue;
			} catch (...) {
				stab.Miss();
				continue;
			}

			const std::string stable = stab.Feed(line);
			if (!stable.empty()) Publish(stable);
		}

		session.Close();
		framePool.Close();
		IMR_LOG_I("videosub: capture stopped");
	}

	// One frame -> one recognised (uncleaned) subtitle candidate.
	std::string RecogniseFrame(WGC::Direct3D11CaptureFrame const& a_frame,
		ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
		WOCR::OcrEngine const& a_engine, const VideoSubConfig& a_cfg)
	{
		auto surface = a_frame.Surface();
		if (!surface) return {};

		winrt::com_ptr<ID3D11Texture2D> tex;
		auto access = surface.as<IDirect3DDxgiInterfaceAccess>();
		winrt::check_hresult(access->GetInterface(
			winrt::guid_of<ID3D11Texture2D>(), tex.put_void()));
		if (!tex) return {};

		D3D11_TEXTURE2D_DESC desc{};
		tex->GetDesc(&desc);
		if (desc.Width < 64 || desc.Height < 64) return {};

		// Keep only the bottom strip where players draw subtitles.
		const int pct = std::clamp(a_cfg.cropBottomPct, 10, 80);
		const UINT cropH = std::max<UINT>(32, desc.Height * static_cast<UINT>(pct) / 100);
		const UINT cropW = desc.Width;
		const UINT cropTop = desc.Height - cropH;

		D3D11_TEXTURE2D_DESC sd{};
		sd.Width = cropW;
		sd.Height = cropH;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.Format = desc.Format;
		sd.SampleDesc.Count = 1;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		winrt::com_ptr<ID3D11Texture2D> staging;
		winrt::check_hresult(a_device->CreateTexture2D(&sd, nullptr, staging.put()));

		D3D11_BOX box{};
		box.left = 0; box.right = cropW;
		box.top = cropTop; box.bottom = desc.Height;
		box.front = 0; box.back = 1;
		a_ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);

		D3D11_MAPPED_SUBRESOURCE mapped{};
		winrt::check_hresult(a_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
		std::vector<std::uint8_t> pixels(
			static_cast<std::size_t>(cropW) * cropH * 4);
		const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
		for (UINT y = 0; y < cropH; ++y) {
			std::memcpy(pixels.data() + static_cast<std::size_t>(y) * cropW * 4,
				src + static_cast<std::size_t>(y) * mapped.RowPitch,
				static_cast<std::size_t>(cropW) * 4);
		}
		a_ctx->Unmap(staging.get(), 0);

		WSS::DataWriter writer;
		writer.WriteBytes(winrt::array_view<const std::uint8_t>(pixels));
		auto bitmap = WGI::SoftwareBitmap::CreateCopyFromBuffer(
			writer.DetachBuffer(), WGI::BitmapPixelFormat::Bgra8,
			static_cast<int>(cropW), static_cast<int>(cropH));

		auto result = a_engine.RecognizeAsync(bitmap).get();
		if (!result) return {};

		std::vector<std::string> lines;
		for (auto const& line : result.Lines()) {
			const std::string t = winrt::to_string(line.Text());
			if (!t.empty()) lines.push_back(t);
		}
		return CleanOcrText(lines);
	}
};

// --------------------------------------------------------------- public ----
VideoSubtitle::~VideoSubtitle()
{
	Stop();
	delete impl_;
	impl_ = nullptr;
}

bool VideoSubtitle::Init(std::string* a_err)
{
	// Static capability probe; safe on the engine thread's MTA (the SMTC
	// Init() above has already initialised the apartment).
	try {
		const bool zh = WOCR::OcrEngine::IsLanguageSupported(WGL::Language(L"zh-Hans"));
		auto any = WOCR::OcrEngine::TryCreateFromUserProfileLanguages();
		available_ = zh || (any != nullptr);
	} catch (...) {
		available_ = false;
	}
	if (!available_ && a_err) {
		*a_err = "no Windows OCR language pack (install Chinese language support)";
	}
	IMR_LOG_I("videosub: OCR available=%d", available_ ? 1 : 0);
	return available_;
}

bool VideoSubtitle::Start(const VideoSubConfig& a_cfg, const std::string& a_hint)
{
	if (!available_) return false;

	HWND hwnd = FindBrowserWindow(a_hint);
	if (!hwnd) {
		IMR_LOG_D("videosub: no browser window visible this frame");
		return false;
	}

	if (impl_ && impl_->worker.joinable() && impl_->target == hwnd) return true;

	Stop();
	if (!impl_) impl_ = new Impl();
	impl_->cfg = a_cfg;
	impl_->hint = a_hint;
	impl_->target = hwnd;
	impl_->stop = false;
	impl_->Clear();
	impl_->worker = std::thread([this] { impl_->Run(); });
	return true;
}

void VideoSubtitle::Stop()
{
	if (!impl_) return;
	{
		std::lock_guard<std::mutex> lk(impl_->mtx);
		impl_->stop = true;
	}
	impl_->cv.notify_all();
	if (impl_->worker.joinable()) impl_->worker.join();
	impl_->Clear();
	impl_->target = nullptr;
}

bool VideoSubtitle::Running() const
{
	return impl_ && impl_->worker.joinable();
}

bool VideoSubtitle::Take(std::string* a_text, double* a_at)
{
	if (!impl_) return false;
	std::lock_guard<std::mutex> lk(impl_->mtx);
	if (impl_->text.empty()) return false;
	if (a_text) *a_text = impl_->text;
	if (a_at) *a_at = impl_->at;
	return true;
}

}  // namespace imr
