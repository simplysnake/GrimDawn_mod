#include "paths.h"

#include <windows.h>

namespace gdvr::paths {

namespace {

std::wstring Env(const wchar_t* name) {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
  return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring Parent(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"/\\");
  return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

}  // namespace

std::wstring InstallDir() {
  static const std::wstring dir = [] {
#ifdef GDVR_DEV_LOG_DIR
    return Parent(GDVR_DEV_LOG_DIR);  // <repo>/logs -> <repo>
#else
    std::wstring home = Env(L"GDVR_HOME");
    if (!home.empty()) return home;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallDir), &self);
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(self, path, MAX_PATH);
    return Parent(path);
#endif
  }();
  return dir;
}

std::wstring UserDir() {
  static const std::wstring dir = [] {
    std::wstring base = Env(L"LOCALAPPDATA");
    std::wstring d = (base.empty() ? std::wstring(L".") : base) + L"\\gdvr";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
  }();
  return dir;
}

}  // namespace gdvr::paths
