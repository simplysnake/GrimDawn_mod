#include "update.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace gdvr::update {

namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kLatestPath[] = L"/repos/simplysnake/GrimDawn_mod/releases?per_page=1";  // newest, pre-releases too
constexpr char kSetupPrefix[] = "GrimDawnVR-Setup-";
constexpr wchar_t kTitle[] = L"Grim Dawn VR";
constexpr long long kCheckEverySeconds = 24 * 3600;

void Logf(const fs::path& userDir, const wchar_t* fmt, const wchar_t* arg = L"") {
  FILE* f = nullptr;
  if (_wfopen_s(&f, (userDir / L"launcher.log").c_str(), L"a, ccs=UTF-8") == 0 && f) {
    fwprintf(f, L"update: ");
    fwprintf(f, fmt, arg);
    fwprintf(f, L"\n");
    fclose(f);
  }
}

std::wstring Widen(const std::string& s) {
  std::wstring out(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), int(out.size()));
  return out;
}

// GET over HTTPS (redirects followed). Appends the body to `body`, or writes it to `file` if given.
bool HttpGet(const std::wstring& url, std::string* body, const fs::path* file, DWORD timeoutMs) {
  URL_COMPONENTS uc{sizeof(uc)};
  wchar_t host[256] = L"", path[2048] = L"";
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
  HINTERNET session = WinHttpOpen(L"GrimDawnVR-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, int(timeoutMs), int(timeoutMs), int(timeoutMs), int(timeoutMs));
  HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                              : nullptr;
  bool ok = request &&
            WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr);
  DWORD status = 0, size = sizeof(status);
  if (ok)
    ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX) &&
         status == 200;
  std::ofstream out;
  if (ok && file) {
    out.open(*file, std::ios::binary | std::ios::trunc);
    ok = bool(out);
  }
  std::vector<char> buf(64 * 1024);
  for (DWORD read = 0; ok;) {
    if (!WinHttpReadData(request, buf.data(), DWORD(buf.size()), &read)) ok = false;
    else if (read == 0) break;
    else if (file) out.write(buf.data(), read);
    else body->append(buf.data(), read);
  }
  if (request) WinHttpCloseHandle(request);
  if (connect) WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return ok && (!file || bool(out));
}

// The string value of "key" at or after `from` in GitHub's JSON (only plain strings are needed here).
bool JsonString(const std::string& json, const char* key, size_t from, std::string& value, size_t& at) {
  const std::string k = std::string("\"") + key + "\"";
  at = json.find(k, from);
  if (at == std::string::npos) return false;
  size_t p = json.find(':', at + k.size());
  if (p == std::string::npos) return false;
  p = json.find_first_not_of(" \t\r\n", p + 1);
  if (p == std::string::npos || json[p] != '"') return false;
  const size_t end = json.find('"', p + 1);
  if (end == std::string::npos) return false;
  value = json.substr(p + 1, end - p - 1);
  return true;
}

// "v1.2.3" / "1.2.3" -> {1, 2, 3}
std::vector<int> ParseVersion(std::wstring v) {
  if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
  std::vector<int> out;
  int n = 0;
  bool digits = false;
  for (wchar_t c : v + L".") {
    if (c >= L'0' && c <= L'9') {
      n = n * 10 + (c - L'0');
      digits = true;
    } else if (c == L'.') {
      out.push_back(digits ? n : 0);
      n = 0;
      digits = false;
    } else {
      break;  // "-beta" and the like: ignore the rest
    }
  }
  while (out.size() < 3) out.push_back(0);
  return out;
}

std::string Sha256Hex(const fs::path& file) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    std::ifstream in(file, std::ios::binary);
    std::vector<char> buf(64 * 1024);
    while (in) {
      in.read(buf.data(), std::streamsize(buf.size()));
      if (in.gcount() > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(in.gcount()), 0);
    }
    unsigned char digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
      char h[3];
      for (unsigned char b : digest) {
        snprintf(h, sizeof(h), "%02x", b);
        hex += h;
      }
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

// The newest release on GitHub, if it is newer than `currentVersion`: its version, installer link and SHA-256.
struct Release {
  std::wstring version;
  std::string url, digest;
};

bool FindNewer(const fs::path& userDir, const wchar_t* currentVersion, Release& rel) {
  std::string json;
  if (!HttpGet(std::wstring(L"https://") + kApiHost + kLatestPath, &json, nullptr, 5000)) {
    Logf(userDir, L"could not reach GitHub (offline?); playing this version");
    return false;
  }
  std::string tag, url, digest;
  size_t at = 0, urlAt = 0, digestAt = 0;
  if (!JsonString(json, "tag_name", 0, tag, at)) return false;
  // The installer's download link, and the checksum GitHub lists for that same file (just before it).
  for (size_t from = 0; JsonString(json, "browser_download_url", from, url, urlAt); from = urlAt + 1) {
    const size_t slash = url.rfind('/');
    if (url.compare(slash + 1, sizeof(kSetupPrefix) - 1, kSetupPrefix) == 0) break;
    url.clear();
  }
  const std::wstring latest = Widen(tag);
  if (ParseVersion(latest) <= ParseVersion(currentVersion)) {
    Logf(userDir, L"up to date (newest release %s)", latest.c_str());
    return false;
  }
  if (url.empty()) {
    Logf(userDir, L"release %s has no installer", latest.c_str());
    return false;
  }
  // The installer's checksum: the last "digest" before its download link (both belong to the same asset).
  std::string d;
  for (size_t from = 0; JsonString(json, "digest", from, d, digestAt) && digestAt < urlAt; from = digestAt + 1) digest = d;
  rel = {latest, url, digest.rfind("sha256:", 0) == 0 ? digest.substr(7) : std::string()};
  return true;
}

}  // namespace

bool Probe(const fs::path& userDir, const wchar_t* asVersion, bool download) {
  Release rel;
  const bool newer = FindNewer(userDir, asVersion, rel);
  if (newer) Logf(userDir, (L"newer: " + rel.version + L" " + Widen(rel.url) + L" sha256 " + Widen(rel.digest)).c_str());
  if (!newer || !download) return newer;
  std::error_code ec;
  fs::create_directories(userDir / L"update", ec);
  const fs::path file = userDir / L"update" / L"probe.exe";
  const bool got = HttpGet(Widen(rel.url), nullptr, &file, 60000);
  const std::string sum = got ? Sha256Hex(file) : std::string();
  Logf(userDir, (L"probe download " + std::wstring(got ? L"ok" : L"FAILED") + L", sha256 " + Widen(sum) +
                 (_stricmp(sum.c_str(), rel.digest.c_str()) == 0 ? L" (matches)" : L" (MISMATCH)")).c_str());
  fs::remove(file, ec);
  return got && _stricmp(sum.c_str(), rel.digest.c_str()) == 0;
}

bool OfferUpdate(const fs::path& userDir, const wchar_t* currentVersion) {
  const std::wstring ini = (userDir / L"launcher.ini").wstring();
  if (GetPrivateProfileIntW(L"update", L"check", 1, ini.c_str()) == 0) return false;
  wchar_t last[32] = L"0";
  GetPrivateProfileStringW(L"update", L"last_check", L"0", last, 32, ini.c_str());
  const long long now = long long(std::time(nullptr));
  if (now - _wtoll(last) < kCheckEverySeconds) return false;
  WritePrivateProfileStringW(L"update", L"last_check", std::to_wstring(now).c_str(), ini.c_str());

  Release rel;
  if (!FindNewer(userDir, currentVersion, rel)) return false;
  wchar_t skip[64] = L"";
  GetPrivateProfileStringW(L"update", L"skip", L"", skip, 64, ini.c_str());
  if (rel.version == skip) return false;
  const std::wstring& latest = rel.version;
  const std::string& url = rel.url;
  const std::string& digest = rel.digest;

  const std::wstring msg = L"Grim Dawn VR " + latest + L" is available (you have " + currentVersion +
                           L").\n\nWhat's new: github.com/simplysnake/GrimDawn_mod/releases\n\n"
                           L"Yes: download and install it now (the game starts afterwards)\n"
                           L"No: play this version now\n"
                           L"Cancel: don't ask about " + latest + L" again";
  const int answer = MessageBoxW(nullptr, msg.c_str(), kTitle, MB_YESNOCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND);
  if (answer == IDCANCEL) {
    WritePrivateProfileStringW(L"update", L"skip", latest.c_str(), ini.c_str());
    return false;
  }
  if (answer != IDYES) return false;

  std::error_code ec;
  const fs::path dir = userDir / L"update";
  fs::create_directories(dir, ec);
  const fs::path setup = dir / Widen(url.substr(url.rfind('/') + 1));
  Logf(userDir, L"downloading %s", Widen(url).c_str());
  if (!HttpGet(Widen(url), nullptr, &setup, 60000)) {
    MessageBoxW(nullptr, L"The download failed. Playing this version; you can get the update from the GitHub "
                         L"releases page.", kTitle, MB_OK | MB_ICONWARNING);
    return false;
  }
  const std::string sum = Sha256Hex(setup);
  if (digest.size() != 64 || _stricmp(sum.c_str(), digest.c_str()) != 0) {  // no checksum: not run either
    fs::remove(setup, ec);
    Logf(userDir, L"checksum mismatch: download discarded");
    MessageBoxW(nullptr, L"The downloaded installer did not match its published checksum, so it was not run. "
                         L"Playing this version.", kTitle, MB_OK | MB_ICONWARNING);
    return false;
  }
  // Upgrade in place; don't redo the Steam entry or the Defender question; start the game again afterwards.
  const std::wstring params = L"/SILENT /MERGETASKS=\"!steam,!defender,!desktopicon\" /RELAUNCH=1";
  const HINSTANCE r = ShellExecuteW(nullptr, L"open", setup.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) <= 32) {
    Logf(userDir, L"could not start the installer");
    return false;
  }
  Logf(userDir, L"installer started: %s", setup.c_str());
  return true;
}

}  // namespace gdvr::update
