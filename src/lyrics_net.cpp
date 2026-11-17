// InteractiveMusicRadio - lyric lookup (WinHTTP -> 网易云 public lyric API).
//
// Kept in its own file because it is the only networked code in the plugin:
// everything it returns is optional, every failure is silent by design, and the
// caller falls back to showing the track line.
#include "lyrics.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <map>
#include <mutex>
#include <string>

#include "core.h"

namespace imr {

namespace {

std::wstring Widen(const std::string& a_utf8)
{
	if (a_utf8.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(),
		static_cast<int>(a_utf8.size()), nullptr, 0);
	std::wstring out(static_cast<std::size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(), static_cast<int>(a_utf8.size()),
		out.data(), n);
	return out;
}

std::string PercentEncode(const std::string& a_utf8)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(a_utf8.size() * 3);
	for (const unsigned char c : a_utf8) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 0x0F];
		}
	}
	return out;
}

bool HttpGet(const std::wstring& a_url, const wchar_t* a_referer, std::string* a_body)
{
	URL_COMPONENTS uc{};
	uc.dwStructSize = sizeof(uc);
	uc.dwSchemeLength = static_cast<DWORD>(-1);
	uc.dwHostNameLength = static_cast<DWORD>(-1);
	uc.dwUrlPathLength = static_cast<DWORD>(-1);
	uc.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(a_url.c_str(), 0, 0, &uc)) return false;

	const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
	std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
	if (uc.lpszExtraInfo) path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);

	HINTERNET session = WinHttpOpen(L"InteractiveMusicRadio/2.0",
		WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) return false;
	WinHttpSetTimeouts(session, 4000, 4000, 6000, 6000);

	HINTERNET connect = WinHttpConnect(session, host.c_str(),
		static_cast<INTERNET_PORT>(uc.nPort ? uc.nPort : 443), 0);
	if (!connect) {
		WinHttpCloseHandle(session);
		return false;
	}

	HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
	if (!request) {
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		return false;
	}

	std::wstring headers = L"Accept: application/json, text/plain, */*\r\n";
	if (a_referer) headers += std::wstring(L"Referer: ") + a_referer + L"\r\n";

	bool ok = WinHttpSendRequest(request, headers.c_str(),
		static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		WinHttpReceiveResponse(request, nullptr);

	if (ok && a_body) {
		DWORD avail = 0;
		do {
			avail = 0;
			if (!WinHttpQueryDataAvailable(request, &avail)) break;
			if (avail == 0) break;
			std::string chunk(avail, '\0');
			DWORD read = 0;
			if (!WinHttpReadData(request, chunk.data(), avail, &read)) break;
			chunk.resize(read);
			a_body->append(chunk);
		} while (avail > 0);
	}

	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);
	return ok;
}

// "…\"id\":1234567,…" -> 1234567
long long FirstIdAfter(const std::string& a_body, const std::string& a_key)
{
	const auto at = a_body.find(a_key);
	if (at == std::string::npos) return 0;
	std::size_t i = at + a_key.size();
	while (i < a_body.size() && !std::isdigit(static_cast<unsigned char>(a_body[i]))) ++i;
	long long v = 0;
	while (i < a_body.size() && std::isdigit(static_cast<unsigned char>(a_body[i]))) {
		v = v * 10 + (a_body[i] - '0');
		++i;
	}
	return v;
}

// Reads a JSON string value out of a flat-enough object; only the escapes that
// actually appear in lyric payloads are handled.
std::string JsonString(const std::string& a_body, const std::string& a_key)
{
	const auto at = a_body.find(a_key);
	if (at == std::string::npos) return {};

	std::size_t i = at + a_key.size();
	while (i < a_body.size() && (a_body[i] == ' ' || a_body[i] == ':')) ++i;
	if (i >= a_body.size() || a_body[i] != '"') return {};
	++i;

	std::string out;
	while (i < a_body.size()) {
		const char c = a_body[i++];
		if (c == '"') break;
		if (c != '\\') {
			out += c;
			continue;
		}
		if (i >= a_body.size()) break;
		const char e = a_body[i++];
		switch (e) {
		case 'n': out += '\n'; break;
		case 'r': out += '\r'; break;
		case 't': out += '\t'; break;
		case '"': out += '"';  break;
		case '\\': out += '\\'; break;
		case '/': out += '/';  break;
		case 'u': {
			// \uXXXX -> UTF-8 (BMP only, which is all a lyric needs)
			if (i + 4 > a_body.size()) { i = a_body.size(); break; }
			unsigned code = 0;
			for (int k = 0; k < 4; ++k) {
				const char h = a_body[i + static_cast<std::size_t>(k)];
				code <<= 4;
				if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
				else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
				else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
			}
			i += 4;
			if (code < 0x80) {
				out += static_cast<char>(code);
			} else if (code < 0x800) {
				out += static_cast<char>(0xC0 | (code >> 6));
				out += static_cast<char>(0x80 | (code & 0x3F));
			} else {
				out += static_cast<char>(0xE0 | (code >> 12));
				out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (code & 0x3F));
			}
			break;
		}
		default: out += e; break;
		}
	}
	return out;
}

// Simple cache: a lyric is fetched once per track per session.
std::mutex& CacheMutex()
{
	static std::mutex m;
	return m;
}

std::map<std::string, std::string>& Cache()
{
	static std::map<std::string, std::string> c;
	return c;
}

}  // namespace

bool FetchLyrics(const std::string& a_title, const std::string& a_artist, std::string* a_lrc)
{
	if (a_title.empty()) return false;

	const std::string query = LyricsQuery(a_title, a_artist);
	{
		std::lock_guard<std::mutex> lk(CacheMutex());
		const auto it = Cache().find(query);
		if (it != Cache().end()) {
			if (a_lrc) *a_lrc = it->second;
			return !it->second.empty();
		}
	}

	const std::string lookupUrl =
		"https://music.163.com/api/search/get/web?csrf_token=&s=" + PercentEncode(query) +
		"&type=1&offset=0&total=true&limit=1";

	std::string body;
	if (!HttpGet(Widen(lookupUrl), L"https://music.163.com/", &body)) {
		IMR_LOG_W("lyrics: lookup request failed for '%s'", query.c_str());
		return false;
	}

	const long long songId = FirstIdAfter(body, "\"id\":");
	if (songId <= 0) {
		IMR_LOG_I("lyrics: no match for '%s'", query.c_str());
		std::lock_guard<std::mutex> lk(CacheMutex());
		Cache()[query] = "";
		return false;
	}

	const std::string lyricUrl = "https://music.163.com/api/song/lyric?id=" +
		std::to_string(songId) + "&lv=1&kv=1&tv=-1";
	std::string lyricBody;
	if (!HttpGet(Widen(lyricUrl), L"https://music.163.com/", &lyricBody)) return false;

	std::string lrc = JsonString(lyricBody, "\"lyric\"");
	if (lrc.find('[') == std::string::npos) {
		// Some tracks only have a translation - use it rather than nothing.
		lrc = JsonString(lyricBody, "\"tlyric\"");
	}

	{
		std::lock_guard<std::mutex> lk(CacheMutex());
		Cache()[query] = lrc;
	}
	if (lrc.empty()) return false;
	if (a_lrc) *a_lrc = lrc;
	IMR_LOG_I("lyrics: '%s' -> %u bytes (id %lld)", query.c_str(),
		static_cast<unsigned>(lrc.size()), songId);
	return true;
}

}  // namespace imr
