// gdvr_keywatch: logs every key press on the PC to find out who sends unexpected keys (for example
// the Windows key that appears when Index B is pressed in Grim Dawn).
//
// Uses Raw Input with RIDEV_INPUTSINK: it only listens, it cannot block or delay any key. For each
// key it logs the device it came from (a real keyboard, a virtual keyboard such as Steam's, or
// "injected" when a program synthesised it) and which program was in front.
//
// Output: console and logs/keywatch.log in the repo. Close the window to stop.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

FILE* g_log = nullptr;

std::wstring DeviceName(HANDLE device) {
  if (!device) return L"(no device: injected by a program)";
  UINT size = 0;
  GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size);
  std::wstring name(size, L'\0');
  if (size && GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &size) != UINT(-1)) {
    name.resize(wcslen(name.c_str()));
    return name;
  }
  return L"(unknown device)";
}

std::wstring ForegroundProgram() {
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  std::wstring out = L"?";
  if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
      const wchar_t* slash = wcsrchr(path, L'\\');
      out = slash ? slash + 1 : path;
    }
    CloseHandle(p);
  }
  return out;
}

void Write(const wchar_t* fmt, ...) {
  wchar_t line[1024];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(line, _TRUNCATE, fmt, args);
  va_end(args);
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t stamp[32];
  swprintf_s(stamp, L"[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  wprintf(L"%s%s\n", stamp, line);
  if (g_log) {
    fwprintf(g_log, L"%s%s\n", stamp, line);
    fflush(g_log);
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INPUT) {
    UINT size = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    std::vector<BYTE> buf(size);
    if (size && GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buf.data(), &size,
                                sizeof(RAWINPUTHEADER)) == size) {
      const auto* ri = reinterpret_cast<const RAWINPUT*>(buf.data());
      if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& k = ri->data.keyboard;
        const bool up = (k.Flags & RI_KEY_BREAK) != 0;
        const bool win = k.VKey == VK_LWIN || k.VKey == VK_RWIN;
        Write(L"key vk 0x%02X scan 0x%02X %-4s extra 0x%08lX  from %s  front: %s%s", k.VKey, k.MakeCode,
              up ? L"up" : L"down", k.ExtraInformation, DeviceName(ri->header.hDevice).c_str(),
              ForegroundProgram().c_str(), win ? L"   <-- WINDOWS KEY" : L"");
      }
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int wmain() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring logPath = exe;
  logPath = logPath.substr(0, logPath.find_last_of(L'\\'));            // ...\bin
  logPath = logPath.substr(0, logPath.find_last_of(L'\\')) + L"\\logs";  // ...\logs
  CreateDirectoryW(logPath.c_str(), nullptr);
  logPath += L"\\keywatch.log";
  _wfopen_s(&g_log, logPath.c_str(), L"a, ccs=UTF-8");

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"gdvr_keywatch";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"gdvr keywatch", 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance,
                              nullptr);
  RAWINPUTDEVICE rid{0x01, 0x06, RIDEV_INPUTSINK, hwnd};  // keyboards, even when this window isn't in front
  if (!hwnd || !RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
    wprintf(L"Could not register for keyboard input (%lu)\n", GetLastError());
    return 1;
  }
  Write(L"keywatch started: logging every key (listen only). Log: %s", logPath.c_str());
  Write(L"Now press B on the right Index controller in Grim Dawn, once. Close this window to stop.");
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
