#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace gdvr {

namespace {

// Each game session starts a fresh log; the one before is kept as gdvr.prev.log. A hot reload of the mod
// into the same game keeps writing to the same log (the marker lives in the process's environment, which
// outlives the DLL). Past kMaxBytes the log stops, so a runaway message can't fill the disk.
constexpr LONGLONG kMaxBytes = 32ll << 20;
constexpr wchar_t kStartedVar[] = L"GDVR_LOG_STARTED";

std::mutex g_mutex;

class LogFile {
 public:
  void Open(const std::wstring& path, bool fresh) {
    if (fresh) {
      std::wstring prev = path;
      prev.insert(prev.size() - 4, L".prev");  // gdvr.log -> gdvr.prev.log
      MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    handle_ = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    if (handle_ != INVALID_HANDLE_VALUE) GetFileSizeEx(handle_, &size);
    written_ = size.QuadPart;
  }
  void Write(const char* line, size_t len) {
    if (handle_ == INVALID_HANDLE_VALUE || full_) return;
    if (written_ + LONGLONG(len) > kMaxBytes) {
      static const char kFull[] = "[log full: nothing more is written this session]\n";
      DWORD n = 0;
      WriteFile(handle_, kFull, DWORD(sizeof(kFull) - 1), &n, nullptr);
      full_ = true;
      return;
    }
    DWORD n = 0;
    WriteFile(handle_, line, DWORD(len), &n, nullptr);  // unbuffered: nothing is lost if the game crashes
    written_ += n;
  }
  ~LogFile() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  LONGLONG written_ = 0;
  bool full_ = false;
};

LogFile g_file;
#ifdef GDVR_DEV_LOG_DIR
LogFile g_devFile;  // development builds also log into the repo, for tools that can't see %LOCALAPPDATA%
#endif

std::wstring LogPath() {
  wchar_t base[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  std::wstring dir = (n && n < MAX_PATH) ? std::wstring(base) + L"\\gdvr" : L".";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\gdvr.log";
}

void OpenOnce() {
  static bool opened = false;
  if (opened) return;
  opened = true;
  const bool fresh = GetEnvironmentVariableW(kStartedVar, nullptr, 0) == 0;
  SetEnvironmentVariableW(kStartedVar, L"1");
  g_file.Open(LogPath(), fresh);
#ifdef GDVR_DEV_LOG_DIR
  CreateDirectoryW(GDVR_DEV_LOG_DIR, nullptr);
  g_devFile.Open(std::wstring(GDVR_DEV_LOG_DIR) + L"/gdvr.log", fresh);
#endif
}

}  // namespace

void Log(const char* fmt, ...) {
  char msg[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  SYSTEMTIME t;
  GetLocalTime(&t);
  char line[1100];
  const int len = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\r\n", t.wHour, t.wMinute, t.wSecond,
                           t.wMilliseconds, msg);

  {
    std::lock_guard lock(g_mutex);
    OpenOnce();
    if (len > 0) {
      const size_t n = std::min(size_t(len), sizeof(line) - 1);
      g_file.Write(line, n);
#ifdef GDVR_DEV_LOG_DIR
      g_devFile.Write(line, n);
#endif
    }
  }
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
}

}  // namespace gdvr
