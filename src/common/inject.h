#pragma once
// Injection helpers shared by gdvr_inject (manual / hot reload) and gdvr_launch (Steam shortcut).

#include <windows.h>

#include <string>

namespace gdvr::inject {

inline constexpr wchar_t kProcessName[] = L"Grim Dawn.exe";

// Returns the PID of a running Grim Dawn, or 0.
DWORD FindGameProcess();

// True once the process has loaded the named module (e.g. L"Engine.dll").
bool HasModule(DWORD pid, const wchar_t* moduleName);

// Unloads a previously injected copy of gdvr (if any). Returns true if one was unloaded.
bool Eject(HANDLE process, DWORD pid);

// Copies gdvr.dll from next to the calling exe into %TEMP% under a unique name and loads it
// into the process (unloading any earlier copy first). Prints progress to stdout.
bool InjectFresh(HANDLE process, DWORD pid);

}  // namespace gdvr::inject
