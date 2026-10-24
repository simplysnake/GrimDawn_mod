#include <windows.h>

#include <atomic>
#include <string>

#include <dbghelp.h>

#include <MinHook.h>

#include "d3dtrace.h"
#include "engine_hooks.h"
#include "input.h"
#include "labels.h"
#include "numbers.h"
#include "pointer.h"
#include "log.h"
#include "mask.h"
#include "overlay.h"
#include "paths.h"
#include "perf.h"
#include "xr.h"

namespace {

HANDLE g_initThread = nullptr;
HANDLE g_watchdog = nullptr;
std::atomic<bool> g_stopping{false};

// Freeze watchdog: if the game stops showing frames for a while (a hang, not a crash: nothing else records
// it), write a dump with every thread's stack, so the place it hangs can be found even after the game is
// closed. Once per hang.
void WriteFreezeDump(const std::wstring& dir, const std::wstring& name) {
  const std::wstring path = dir + L"\\" + name;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  const auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
  const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr);
  CloseHandle(file);
  gdvr::Log("freeze watchdog: no frame for 6 s, %s %ls (graphics device: %s 0x%08lX)", ok ? "dump written" : "dump FAILED",
            path.c_str(), gdvr::xr::DeviceRemovedReason() ? "LOST" : "ok", static_cast<unsigned long>(gdvr::xr::DeviceRemovedReason()));
}

DWORD WINAPI Watchdog(LPVOID) {
  constexpr ULONGLONG kStallMs = 6000;
  bool dumped = false;
  while (!g_stopping) {
    Sleep(1000);
    const ULONGLONG last = gdvr::overlay::LastPresentTick();
    if (!last) continue;
    const ULONGLONG stalled = GetTickCount64() - last;
    if (stalled < kStallMs) {
      dumped = false;
      continue;
    }
    if (dumped) continue;
    dumped = true;  // (the dump first: a hung thread might hold the log's lock)
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"freeze_%04d%02d%02d-%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
#ifdef GDVR_DEV_LOG_DIR
    WriteFreezeDump(GDVR_DEV_LOG_DIR, name);
#else
    WriteFreezeDump(gdvr::paths::UserDir(), name);
#endif
  }
  return 0;
}

DWORD WINAPI InitThread(LPVOID) {
  gdvr::Log("gdvr " GDVR_VERSION " loading");
  if (MH_Initialize() != MH_OK) {
    gdvr::Log("MH_Initialize failed");
    return 1;
  }
  const bool engineOk = gdvr::engine::Install();
  const bool overlayOk = gdvr::overlay::Install();
  // The game's input modules (DirectInput.dll, xinput1_3.dll) may load after the mod is injected.
  auto loaded = [](const wchar_t* name) { return GetModuleHandleW(name) != nullptr; };
  bool pointerOk = loaded(L"DirectInput.dll") && gdvr::pointer::Install();
  bool guardOk = loaded(L"DirectInput.dll") && gdvr::input::InstallKeyboardGuard();
  bool xinputOk = loaded(L"xinput1_3.dll") && gdvr::input::InstallXInputHook();
  MH_EnableHook(MH_ALL_HOOKS);
  gdvr::Log("gdvr ready (engine hooks: %s, overlay: %s, input: %s). Press F8 in game.", engineOk ? "ok" : "FAILED",
            overlayOk ? "ok" : "FAILED", pointerOk && guardOk && xinputOk ? "ok" : "waiting for the game's input modules");

  // Hook the input modules as soon as the game loads them (checked twice a second, for 5 minutes).
  for (int i = 0; i < 600 && !(pointerOk && guardOk && xinputOk) && !g_stopping; ++i) {
    Sleep(500);
    if (g_stopping) break;
    bool added = false;
    if (!pointerOk && loaded(L"DirectInput.dll")) added = pointerOk = gdvr::pointer::Install();
    if (!guardOk && loaded(L"DirectInput.dll")) added |= guardOk = gdvr::input::InstallKeyboardGuard();
    if (!xinputOk && loaded(L"xinput1_3.dll")) added |= xinputOk = gdvr::input::InstallXInputHook();
    if (added) {
      MH_EnableHook(MH_ALL_HOOKS);
      gdvr::Log("input hooks added late: mouse/keyboard feed %s, keyboard guard %s, gamepad merge %s",
                pointerOk ? "ok" : "no", guardOk ? "ok" : "no", xinputOk ? "ok" : "no");
    }
  }
  if (!(pointerOk && guardOk && xinputOk) && !g_stopping)
    gdvr::Log("input hooks incomplete: mouse/keyboard feed %s, keyboard guard %s, gamepad merge %s",
              pointerOk ? "ok" : "MISSING", guardOk ? "ok" : "MISSING", xinputOk ? "ok" : "MISSING");
  return 0;
}

}  // namespace

// Called by the injector (via CreateRemoteThread) before it FreeLibrary's us, so a fresh
// build can be loaded into the running game.
extern "C" __declspec(dllexport) DWORD WINAPI GDVR_Shutdown(LPVOID) {
  gdvr::Log("gdvr shutting down");
  g_stopping = true;  // the init thread may still be waiting for the game's input modules
  if (g_watchdog) {
    WaitForSingleObject(g_watchdog, 2000);
    CloseHandle(g_watchdog);
    g_watchdog = nullptr;
  }
  if (g_initThread) {
    WaitForSingleObject(g_initThread, 2000);
    CloseHandle(g_initThread);
    g_initThread = nullptr;
  }
  // The D3D11 immediate context and the OpenXR session belong to the render thread: release
  // them there. Only if the game is not presenting (paused, minimised) do it from here.
  if (!gdvr::overlay::RequestTeardown(3000)) {
    gdvr::Log("game not presenting; tearing down from the unload thread");
    gdvr::overlay::TeardownNow();
  }
  MH_DisableHook(MH_ALL_HOOKS);
  // Wait until no thread is still executing inside one of our detours.
  for (int i = 0; i < 200 && (gdvr::engine::InFlight() || gdvr::overlay::InFlight() || gdvr::perf::InFlight() ||
                              gdvr::mask::InFlight() || gdvr::input::InFlight() || gdvr::labels::InFlight() || gdvr::numbers::InFlight() || gdvr::pointer::InFlight() ||
                              gdvr::d3dtrace::InFlight()); ++i) Sleep(10);
  Sleep(50);  // also lets threads that were between the trampoline and the guard drain
  MH_Uninitialize();
  gdvr::Log("gdvr unloaded");
  return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    g_initThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    g_watchdog = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
  }
  return TRUE;
}
