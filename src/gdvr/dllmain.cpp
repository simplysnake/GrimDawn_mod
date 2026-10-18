#include <windows.h>

#include <atomic>

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
#include "perf.h"
#include "xr.h"

namespace {

HANDLE g_initThread = nullptr;
std::atomic<bool> g_stopping{false};

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
  }
  return TRUE;
}
