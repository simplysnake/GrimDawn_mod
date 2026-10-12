// gdvr_inject: loads gdvr.dll into a running Grim Dawn, or reloads it after a rebuild.
//
//   gdvr_inject           inject (unloads any previously injected copy first = hot reload)
//   gdvr_inject --eject   unload only

#include <windows.h>

#include <cstdio>

#include "../common/inject.h"

int wmain(int argc, wchar_t** argv) {
  using namespace gdvr::inject;
  const bool ejectOnly = argc > 1 && _wcsicmp(argv[1], L"--eject") == 0;

  DWORD pid = FindGameProcess();
  if (!pid) {
    wprintf(L"%s is not running. Start the game first.\n", kProcessName);
    return 1;
  }
  HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  if (!process) {
    wprintf(L"OpenProcess failed (%lu).\n", GetLastError());
    return 1;
  }

  bool ok = ejectOnly ? (Eject(process, pid), true) : InjectFresh(process, pid);
  CloseHandle(process);
  return ok ? 0 : 1;
}
