#include <windows.h>

#include <MinHook.h>

#include "engine_hooks.h"
#include "log.h"
#include "overlay.h"
#include "perf.h"
#include "xr.h"

namespace {

DWORD WINAPI InitThread(LPVOID) {
  gdvr::Log("gdvr loading");
  if (MH_Initialize() != MH_OK) {
    gdvr::Log("MH_Initialize failed");
    return 1;
  }
  const bool engineOk = gdvr::engine::Install();
  const bool overlayOk = gdvr::overlay::Install();
  MH_EnableHook(MH_ALL_HOOKS);
  gdvr::Log("gdvr ready (engine hooks: %s, overlay: %s). Press F8 in game.", engineOk ? "ok" : "FAILED",
            overlayOk ? "ok" : "FAILED");
  return 0;
}

}  // namespace

// Called by the injector (via CreateRemoteThread) before it FreeLibrary's us, so a fresh
// build can be loaded into the running game.
extern "C" __declspec(dllexport) DWORD WINAPI GDVR_Shutdown(LPVOID) {
  gdvr::Log("gdvr shutting down");
  MH_DisableHook(MH_ALL_HOOKS);
  // Wait until no thread is still executing inside one of our detours.
  for (int i = 0; i < 200 && (gdvr::engine::InFlight() || gdvr::overlay::InFlight() || gdvr::perf::InFlight()); ++i) Sleep(10);
  Sleep(50);  // also lets threads that were between the trampoline and the guard drain
  gdvr::xr::Shutdown();
  gdvr::perf::Shutdown();
  gdvr::overlay::Shutdown();
  MH_Uninitialize();
  gdvr::Log("gdvr unloaded");
  return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
  }
  return TRUE;
}
