#include "inject.h"

#include <tlhelp32.h>

#include <cstdio>

namespace gdvr::inject {

namespace {

constexpr wchar_t kDllName[] = L"gdvr.dll";
constexpr wchar_t kCopyPrefix[] = L"gdvr_";
constexpr char kShutdownExport[] = "GDVR_Shutdown";

// Returns the first module in the target whose name starts with gdvr_ (a copy we injected earlier).
bool FindInjected(DWORD pid, HMODULE& base, std::wstring& path) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return false;
  MODULEENTRY32W me = {sizeof(me)};
  bool found = false;
  for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me)) {
    if (_wcsnicmp(me.szModule, kCopyPrefix, wcslen(kCopyPrefix)) == 0) {
      base = me.hModule;
      path = me.szExePath;
      found = true;
    }
  }
  CloseHandle(snap);
  return found;
}

constexpr DWORD kRemoteTimeout = DWORD(-2);

DWORD RunRemote(HANDLE process, void* fn, void* arg) {
  HANDLE t = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(fn), arg, 0, nullptr);
  if (!t) return DWORD(-1);
  if (WaitForSingleObject(t, 15000) != WAIT_OBJECT_0) {
    CloseHandle(t);
    return kRemoteTimeout;
  }
  DWORD code = 0;
  GetExitCodeThread(t, &code);
  CloseHandle(t);
  return code;
}

bool LoadIntoProcess(HANDLE process, const std::wstring& dllPath) {
  const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote || !WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) return false;
  // kernel32 is mapped at the same address in every process of a session.
  DWORD rc = RunRemote(process, reinterpret_cast<void*>(&LoadLibraryW), remote);
  VirtualFreeEx(process, remote, 0, MEM_RELEASE);
  return rc != 0 && rc != DWORD(-1);
}

}  // namespace

DWORD FindGameProcess() {
  DWORD pid = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W pe = {sizeof(pe)};
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    if (_wcsicmp(pe.szExeFile, kProcessName) == 0) pid = pe.th32ProcessID;
  CloseHandle(snap);
  return pid;
}

bool HasModule(DWORD pid, const wchar_t* moduleName) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return false;
  MODULEENTRY32W me = {sizeof(me)};
  bool found = false;
  for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
    found = _wcsicmp(me.szModule, moduleName) == 0;
  CloseHandle(snap);
  return found;
}

bool Eject(HANDLE process, DWORD pid) {
  HMODULE remoteBase;
  std::wstring path;
  if (!FindInjected(pid, remoteBase, path)) return false;

  // Locate GDVR_Shutdown in the target: same offset from the module base as in a local mapping.
  HMODULE local = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (local) {
    auto* fn = reinterpret_cast<BYTE*>(GetProcAddress(local, kShutdownExport));
    if (fn) {
      void* remoteFn = reinterpret_cast<BYTE*>(remoteBase) + (fn - reinterpret_cast<BYTE*>(local));
      wprintf(L"Shutting down %s...\n", path.c_str());
      if (RunRemote(process, remoteFn, nullptr) == kRemoteTimeout) {
        // Freeing it now would leave the game calling into unloaded code.
        wprintf(L"Shutdown did not finish within 15 s; leaving the old copy loaded. Restart the game.\n");
        FreeLibrary(local);
        return false;
      }
    }
    FreeLibrary(local);
  }
  RunRemote(process, reinterpret_cast<void*>(&FreeLibrary), remoteBase);
  DeleteFileW(path.c_str());  // may fail if still mapped; harmless
  wprintf(L"Unloaded previous copy.\n");
  return true;
}

bool InjectFresh(HANDLE process, DWORD pid) {
  HMODULE existing;
  std::wstring existingPath;
  if (FindInjected(pid, existing, existingPath) && !Eject(process, pid)) return false;

  // Source DLL sits next to the calling exe.
  wchar_t exePath[MAX_PATH];
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  std::wstring src(exePath);
  src = src.substr(0, src.find_last_of(L"\\/") + 1) + kDllName;

  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring copy = std::wstring(temp) + kCopyPrefix + std::to_wstring(GetTickCount64()) + L".dll";
  if (!CopyFileW(src.c_str(), copy.c_str(), FALSE)) {
    wprintf(L"Could not copy %s (%lu). Did you build it?\n", src.c_str(), GetLastError());
    return false;
  }

  const bool ok = LoadIntoProcess(process, copy);
  wprintf(ok ? L"Injected %s. Press F8 in game; log: %%LOCALAPPDATA%%\\gdvr\\gdvr.log\n"
             : L"Injection of %s failed.\n",
          copy.c_str());
  return ok;
}

}  // namespace gdvr::inject
