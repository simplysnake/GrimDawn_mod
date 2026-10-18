// dinput8.dll stand-in, placed in Grim Dawn's x64 folder: how the mod gets into the game without
// injecting it (injectors look like malware to antivirus software).
//
// The game's DirectInput.dll loads dinput8.dll from the game folder before System32, so it gets this one.
// Every call is passed on to the real dinput8.dll in System32. Only when the game was started by the
// mod's launcher (which sets GDVR_DLL) does it also load the mod; started any other way, the game runs
// exactly as it does without it.

#include <windows.h>
#include <unknwn.h>

#include <mutex>

namespace {

HMODULE g_real = nullptr;
std::once_flag g_realOnce;

FARPROC Real(const char* name) {
  std::call_once(g_realOnce, [] {
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 16) {
      wcscat_s(path, L"\\dinput8.dll");
      g_real = LoadLibraryW(path);
    }
  });
  return g_real ? GetProcAddress(g_real, name) : nullptr;
}

// Loads the mod (on its own thread: not while the loader is busy loading this DLL).
DWORD WINAPI LoadMod(LPVOID path) {
  if (!LoadLibraryW(static_cast<const wchar_t*>(path))) {
    wchar_t msg[MAX_PATH + 128];
    swprintf_s(msg, L"Grim Dawn VR: could not load the mod\n%s\n(error %lu)", static_cast<const wchar_t*>(path),
               GetLastError());
    MessageBoxW(nullptr, msg, L"Grim Dawn VR", MB_OK | MB_ICONWARNING);
  }
  return 0;
}

bool IsGameProcess() {
  wchar_t exe[MAX_PATH] = L"";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const wchar_t* name = wcsrchr(exe, L'\\');
  return _wcsicmp(name ? name + 1 : exe, L"Grim Dawn.exe") == 0;
}

wchar_t g_modPath[MAX_PATH];

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID* out, IUnknown* outer) {
  using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
  auto fn = reinterpret_cast<Fn>(Real("DirectInput8Create"));
  return fn ? fn(inst, version, iid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
  auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllCanUnloadNow"));
  return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* out) {
  auto fn = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(Real("DllGetClassObject"));
  return fn ? fn(clsid, iid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
  auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllRegisterServer"));
  return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
  auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllUnregisterServer"));
  return fn ? fn() : E_FAIL;
}

LPCVOID WINAPI Proxy_GetdfDIJoystick() {
  auto fn = reinterpret_cast<LPCVOID(WINAPI*)()>(Real("GetdfDIJoystick"));
  return fn ? fn() : nullptr;
}

// Tells the launcher this dinput8.dll is the mod's own (not another mod's): it searches the file for this name.
const char* WINAPI GDVR_ProxyVersion() { return GDVR_VERSION; }

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    const DWORD n = GetEnvironmentVariableW(L"GDVR_DLL", g_modPath, MAX_PATH);
    if (n > 0 && n < MAX_PATH && IsGameProcess())
      if (HANDLE t = CreateThread(nullptr, 0, LoadMod, g_modPath, 0, nullptr)) CloseHandle(t);
  }
  return TRUE;
}
