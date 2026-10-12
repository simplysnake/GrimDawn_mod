#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace gdvr {

namespace {
std::mutex g_mutex;

std::wstring LogPath() {
  wchar_t base[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  std::wstring dir = (n && n < MAX_PATH) ? std::wstring(base) + L"\\gdvr" : L".";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\gdvr.log";
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

  std::lock_guard lock(g_mutex);
  static const std::wstring path = LogPath();
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"a") == 0 && f) {
    fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    fclose(f);
  }
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
}

}  // namespace gdvr
