#include "pointer.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <intrin.h>

#include <MinHook.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "input.h"
#include "log.h"
#include "mask.h"
#include "overlay.h"
#include "xr.h"

namespace gdvr::pointer {

namespace {

std::atomic<int> g_inFlight{0};
std::atomic<ULONGLONG> g_lastUpdate{0};

// ---- This frame's pointer ---------------------------------------------------------------------
struct State {
  bool valid = false;       // a tracked controller ray this frame
  Vec3 start, end;          // laser in game space
  bool onGround = false;    // `end` is a ground hit (draw the ring there)
  bool onPanel = false;     // the ray hits the HUD panel first
  float panelU = 0, panelV = 0;
  bool haveClient = false;  // the laser as a position on the game's screen (window client pixels)
  float clientX = 0, clientY = 0;
  bool moving = false;      // left stick: walking by holding a click ahead of the character
  bool pulled = false;      // the laser spot is off the game's screen: the click is pulled in
};
std::mutex g_mutex;
State g_state;
// HUD pointing (render thread): smoothing, stickiness and the click lock.
bool g_onPanelLast = false;
std::atomic<double> g_parkUntil{0};
  // cursor parked off-screen until (NowMs)
float g_smoothX = 0, g_smoothY = 0;
bool g_clickWasDown = false;
double g_lockUntil = 0;
float g_lockX = 0, g_lockY = 0;

constexpr float kMaxLength = 60.0f;   // game units, when the ray hits nothing
constexpr float kRingRadius = 0.45f;  // game units
constexpr float kMaxReach = 45.0f;    // ground hits further than this from the character are cut short
constexpr float kMoveAhead = 6.0f;    // game units ahead of the character the stick "clicks"

// ---- Mouse and keyboard the mod gives the game -------------------------------------------------
// The game's DirectInput.dll reads the cursor with GetCursorPos (then ScreenToClient), the mouse
// buttons and keys as held state (IDirectInputDevice8::GetDeviceState: DIMOUSESTATE2 / 256 keys)
// and as press/release events (GetDeviceData). The mod adds its state to both; when the devices
// aren't acquired (game window not focused while you play in the headset) it supplies them alone.
struct Output {
  bool keys[256] = {};  // DIK_* codes held
  bool buttons[2] = {};  // left, right mouse button
};
std::mutex g_outMutex;
Output g_out;                              // what the game should see now (under g_outMutex)
bool g_keysSent[256] = {}, g_buttonsSent[2] = {};  // what press/release events have told it (under g_outMutex)
std::atomic<bool> g_active{false};         // pointer mode: the mod drives the game's mouse and keys
std::atomic<bool> g_feeding{false};        // the cursor position comes from the mod
std::atomic<long> g_feedX{0}, g_feedY{0};  // client pixels
std::atomic<long> g_motionX{0}, g_motionY{0};  // mouse movement to report (so the game sees a mouse)
std::atomic<void*> g_keyboardDevice{nullptr}, g_mouseDevice{nullptr};
std::atomic<bool> g_keyboardChecked{false};  // the game's own devices are known (Update hook)
std::atomic<DWORD> g_sequence{0x40000000};
std::atomic<unsigned long long> g_eventsSent{0};
// Keys the mod taps itself (TapKey): pressed until the time given, applied directly in the hooks so a
// tap is always released, whatever the rendering does (the pointer update stops in flat menus).
std::atomic<double> g_forceUntil[256] = {};
double HookNowMs() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}
bool Forced(int k, double now) { return now < g_forceUntil[k].load(); }
bool AnyForced(double now) {
  for (int k = 0; k < 256; ++k)
    if (Forced(k, now)) return true;
  return false;
}

HMODULE g_gameInput = nullptr;
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
GetCursorPosFn g_origGetCursorPos = nullptr;
using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD size, void* data);
using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD objectSize, DIDEVICEOBJECTDATA* data,
                                                     DWORD* count, DWORD flags);
GetDeviceStateFn g_origStateA = nullptr, g_origStateW = nullptr;
GetDeviceDataFn g_origDataA = nullptr, g_origDataW = nullptr;

bool FromGameInput(void* returnAddress) {
  HMODULE mod = nullptr;
  return g_gameInput &&
         GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(returnAddress), &mod) &&
         mod == g_gameInput;
}

// The game's own modules (besides DirectInput.dll) may read the cursor too, e.g. in menus that pause
// the game: they get the pointer's position as well.
HMODULE g_gameModules[3] = {};  // GrimDawn.exe, Engine.dll, Game.dll
bool FromGame(void* returnAddress, HMODULE& mod) {
  mod = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(returnAddress), &mod))
    return false;
  if (mod == g_gameInput) return true;
  for (HMODULE m : g_gameModules)
    if (m && mod == m) return true;
  return false;
}
// Who asks for the cursor (diagnostic, logged with the pointer status).
struct CursorCaller {
  std::atomic<HMODULE> mod{nullptr};
  std::atomic<unsigned long long> calls{0}, fed{0};
};
CursorCaller g_cursorCallers[8];
void CountCursorCaller(HMODULE mod, bool fed) {
  for (CursorCaller& c : g_cursorCallers) {
    HMODULE expected = nullptr;
    if (c.mod.load() == mod || c.mod.compare_exchange_strong(expected, mod) || expected == mod) {
      ++c.calls;
      if (fed) ++c.fed;
      return;
    }
  }
}

BOOL WINAPI HookGetCursorPos(LPPOINT point) {
  ++g_inFlight;
  BOOL ok = FALSE;
  bool fed = false;
  HMODULE caller;
  const bool fromGame = FromGame(_ReturnAddress(), caller);
  if (point && g_active && g_feeding && fromGame) {
    if (HWND window = overlay::GameWindow()) {
      POINT p{g_feedX.load(), g_feedY.load()};
      if (ClientToScreen(window, &p)) {
        *point = p;
        ok = fed = TRUE;
      }
    }
  }
  if (!fed) ok = g_origGetCursorPos(point);
  CountCursorCaller(caller, fed);
  --g_inFlight;
  return ok;
}

HRESULT OnDeviceState(void* self, DWORD size, void* data, HRESULT hr, void* returnAddress) {
  const bool keyboard = size == 256, mouse = size == sizeof(DIMOUSESTATE2) || size == sizeof(DIMOUSESTATE);
  if (!data || !(keyboard || mouse) || !FromGameInput(returnAddress)) return hr;
  if (!g_keyboardChecked) (keyboard ? g_keyboardDevice : g_mouseDevice) = self;
  const double now = HookNowMs();
  if (!g_active && !(keyboard && AnyForced(now))) return hr;
  if (FAILED(hr)) {
    std::memset(data, 0, size);
    hr = DI_OK;
  }
  std::lock_guard lock(g_outMutex);
  if (keyboard) {
    auto* keys = static_cast<BYTE*>(data);
    for (int k = 0; k < 256; ++k)
      if ((g_active && g_out.keys[k]) || Forced(k, now)) keys[k] = 0x80;
  } else {
    auto* m = static_cast<DIMOUSESTATE*>(data);
    m->lX += g_motionX.exchange(0);
    m->lY += g_motionY.exchange(0);
    for (int b = 0; b < 2; ++b)
      if (g_out.buttons[b]) m->rgbButtons[b] = 0x80;
  }
  return hr;
}

HRESULT OnDeviceData(void* self, DWORD objectSize, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags, DWORD capacity,
                     HRESULT hr, void* returnAddress) {
  const bool keyboard = self == g_keyboardDevice.load(), mouse = self == g_mouseDevice.load();
  const double now = HookNowMs();
  const bool forcing = keyboard && AnyForced(now);
  if (!(g_active || forcing) || !(keyboard || mouse) || !data || !count || objectSize != sizeof(DIDEVICEOBJECTDATA) ||
      (flags & DIGDD_PEEK) || !FromGameInput(returnAddress))
    return hr;
  DWORD n = SUCCEEDED(hr) ? *count : 0;
  if (FAILED(hr)) hr = DI_OK;
  auto add = [&](DWORD offset, bool down) {
    if (n >= capacity) return false;
    DIDEVICEOBJECTDATA& e = data[n++];
    e.dwOfs = offset;
    e.dwData = down ? 0x80 : 0;
    e.dwTimeStamp = GetTickCount();
    e.dwSequence = ++g_sequence;
    e.uAppData = UINT_PTR(-1);
    ++g_eventsSent;
    return true;
  };
  std::lock_guard lock(g_outMutex);
  if (keyboard) {
    for (int k = 0; k < 256; ++k) {
      const bool want = (g_active && g_out.keys[k]) || Forced(k, now);
      if (want != g_keysSent[k] && add(DWORD(k), want)) g_keysSent[k] = want;
    }
  } else {
    for (int b = 0; b < 2; ++b)
      if (g_out.buttons[b] != g_buttonsSent[b] && add(DWORD(DIMOFS_BUTTON0 + b), g_out.buttons[b]))
        g_buttonsSent[b] = g_out.buttons[b];
  }
  *count = n;
  return hr;
}

HRESULT STDMETHODCALLTYPE HookStateA(void* self, DWORD size, void* data) {
  ++g_inFlight;
  const HRESULT hr = OnDeviceState(self, size, data, g_origStateA(self, size, data), _ReturnAddress());
  --g_inFlight;
  return hr;
}
HRESULT STDMETHODCALLTYPE HookStateW(void* self, DWORD size, void* data) {
  ++g_inFlight;
  const HRESULT hr = OnDeviceState(self, size, data, g_origStateW(self, size, data), _ReturnAddress());
  --g_inFlight;
  return hr;
}
HRESULT STDMETHODCALLTYPE HookDataA(void* self, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags) {
  ++g_inFlight;
  const DWORD capacity = count ? *count : 0;
  const HRESULT hr = OnDeviceData(self, size, data, count, flags, capacity, g_origDataA(self, size, data, count, flags),
                                  _ReturnAddress());
  --g_inFlight;
  return hr;
}
HRESULT STDMETHODCALLTYPE HookDataW(void* self, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags) {
  ++g_inFlight;
  const DWORD capacity = count ? *count : 0;
  const HRESULT hr = OnDeviceData(self, size, data, count, flags, capacity, g_origDataW(self, size, data, count, flags),
                                  _ReturnAddress());
  --g_inFlight;
  return hr;
}

GetDeviceStateFn g_origStateK = nullptr;
GetDeviceDataFn g_origDataK = nullptr;
HRESULT STDMETHODCALLTYPE HookStateK(void* self, DWORD size, void* data) {
  ++g_inFlight;
  const HRESULT hr = OnDeviceState(self, size, data, g_origStateK(self, size, data), _ReturnAddress());
  --g_inFlight;
  return hr;
}
HRESULT STDMETHODCALLTYPE HookDataK(void* self, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags) {
  ++g_inFlight;
  const DWORD capacity = count ? *count : 0;
  const HRESULT hr = OnDeviceData(self, size, data, count, flags, capacity, g_origDataK(self, size, data, count, flags),
                                  _ReturnAddress());
  --g_inFlight;
  return hr;
}

// ---- The game's own devices ------------------------------------------------------------------------
// DirectInputDevice (DirectInput.dll) keeps its IDirectInputDevice8 keyboard at +0x60 and mouse at
// +0x270. Its per-frame Update tells the mod exactly which devices those are, and lets it hook the
// keyboard's own methods (dinput8 implements keyboards separately from mice).
constexpr char kUpdate[] = "?Update@DirectInputDevice@GAME@@UEAAXXZ";
using UpdateFn = void (*)(void* self);
UpdateFn g_origUpdate = nullptr;
void* g_hookedState[4] = {};  // GetDeviceState / GetDeviceData functions already hooked
void* g_hookedData[4] = {};

bool Hooked(void* const (&list)[4], void* fn) {
  for (void* h : list)
    if (h == fn) return true;
  return false;
}

void CheckGameDevices(void* self) {
  void* keyboard = nullptr;
  void* mouse = nullptr;
  __try {
    keyboard = *reinterpret_cast<void**>(static_cast<char*>(self) + 0x60);
    mouse = *reinterpret_cast<void**>(static_cast<char*>(self) + 0x270);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }
  if (keyboard) g_keyboardDevice = keyboard;
  if (mouse) g_mouseDevice = mouse;
  if (!keyboard || g_keyboardChecked.exchange(true)) return;
  void** vt = *static_cast<void***>(keyboard);
  void* state = vt[9];
  void* data = vt[10];
  bool added = false;
  if (!Hooked(g_hookedState, state) &&
      MH_CreateHook(state, reinterpret_cast<void*>(&HookStateK), reinterpret_cast<void**>(&g_origStateK)) == MH_OK) {
    MH_EnableHook(state);
    added = true;
  }
  if (!Hooked(g_hookedData, data) &&
      MH_CreateHook(data, reinterpret_cast<void*>(&HookDataK), reinterpret_cast<void**>(&g_origDataK)) == MH_OK) {
    MH_EnableHook(data);
    added = true;
  }
  Log("pointer: game keyboard device %p, mouse %p: keyboard methods %s", keyboard, mouse,
      added ? "differ from the mouse's: hooked separately" : "shared with the mouse");
}

void HookUpdate(void* self) {
  ++g_inFlight;
  CheckGameDevices(self);
  g_origUpdate(self);
  --g_inFlight;
}

// ---- Diagnostic: what the game makes of a mouse click -------------------------------------------
// bool ControllerPlayer::HandleActionFromMouse(bool, bool, bool, bool, const WorldVec3& where, uint&, bool*)
constexpr char kHandleActionFromMouse[] =
    "?HandleActionFromMouse@ControllerPlayer@GAME@@QEAA_N_N000AEBVWorldVec3@2@AEAIPEA_N@Z";
using HandleActionFn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, const void*, void*, void*);
HandleActionFn g_origHandleAction = nullptr;
std::atomic<unsigned long long> g_actions{0};

uintptr_t HookHandleAction(void* self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, const void* where,
                           void* id, void* flag) {
  ++g_inFlight;
  const uintptr_t r = g_origHandleAction(self, a, b, c, d, where, id, flag);
  ++g_actions;
  static ULONGLONG lastLog = 0;
  if (GetTickCount64() - lastLog > 250) {
    lastLog = GetTickCount64();
    float p[3] = {};
    if (where) std::memcpy(p, static_cast<const char*>(where) + 8, sizeof(p));
    Log("pointer: game mouse action flags %d %d %d %d at (%.1f %.1f %.1f) -> %d", int(a & 0xFF), int(b & 0xFF),
        int(c & 0xFF), int(d & 0xFF), p[0], p[1], p[2], int(r & 0xFF));
  }
  --g_inFlight;
  return r;
}

// ---- Diagnostic: key events the game's input module produces --------------------------------------
// ButtonEvent DirectInputDevice::GetKeyEvent(int) const (returned through a hidden result pointer).
constexpr char kGetKeyEvent[] = "?GetKeyEvent@DirectInputDevice@GAME@@UEBA?AVButtonEvent@InputDevice@2@H@Z";
using GetKeyEventFn = void* (*)(const void* self, void* result, uintptr_t index);
GetKeyEventFn g_origGetKeyEvent = nullptr;
std::atomic<int> g_keyEventLogs{0};

void* HookGetKeyEvent(const void* self, void* result, uintptr_t index) {
  ++g_inFlight;
  void* r = g_origGetKeyEvent(self, result, index);
  const auto* code = static_cast<const unsigned char*>(result) + 8;
  const bool ours = r == result && (*code == DIK_1 || *code == DIK_2 || *code == DIK_3 || *code == DIK_4 ||
                                    *code == DIK_5 || *code == DIK_6 || *code == DIK_7 || *code == DIK_8 ||
                                    *code == DIK_R || *code == DIK_E || *code == DIK_W || *code == DIK_Y ||
                                    *code == DIK_ESCAPE || *code == DIK_I || *code == DIK_M || *code == DIK_C);
  if (ours && g_keyEventLogs < 200) {
    ++g_keyEventLogs;
    const auto* b = static_cast<const unsigned char*>(result);
    char hex[24 * 3 + 1] = {};
    for (int i = 0; i < 24; ++i) snprintf(hex + i * 3, 4, "%02X ", b[i]);
    Log("pointer: game key event #%d: %s", int(index), hex);
  }
  --g_inFlight;
  return r;
}

// ---- Keep input alive when the game window loses focus (VR) ---------------------------------------
// When its window loses focus the game calls DirectInputDevice::Suspend: it unacquires the devices
// and stops reading them, which also stops everything the mod feeds it. In VR the window is often
// not in front (SteamVR, other windows), so Suspend is skipped while stereo frames are running.
constexpr char kSuspend[] = "?Suspend@DirectInputDevice@GAME@@UEAAXXZ";
using SuspendFn = void (*)(void* self);
SuspendFn g_origSuspend = nullptr;
std::atomic<unsigned long long> g_suspendsSkipped{0};

bool LiveNow();  // below

void HookSuspend(void* self) {
  ++g_inFlight;
  if (LiveNow() && xr::GetSettings().keepRunningUnfocused) {
    if (g_suspendsSkipped++ == 0) Log("pointer: kept the game's input running while its window is unfocused (VR)");
  } else {
    g_origSuspend(self);
  }
  --g_inFlight;
}

// The device methods live in dinput8's device class; a throwaway mouse device gives their addresses.
bool DeviceFunctions(REFIID iid, void*& state, void*& dataFn) {
  state = dataFn = nullptr;
  void* di = nullptr;
  if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid, &di, nullptr)) || !di) return false;
  auto* d = static_cast<IDirectInput8W*>(di);  // same vtable layout for A and W
  IDirectInputDevice8W* device = nullptr;
  if (SUCCEEDED(d->CreateDevice(GUID_SysMouse, &device, nullptr)) && device) {
    void** vt = *reinterpret_cast<void***>(device);
    state = vt[9];    // GetDeviceState
    dataFn = vt[10];  // GetDeviceData
    device->Release();
  }
  d->Release();
  return state && dataFn;
}

// ---- Controller layout (pointer mode) ----------------------------------------------------------
// Right trigger = left click at the laser, left trigger = right click; left stick walks; face
// buttons hotbar 1-4; right-stick flicks hotbar 5-8; grips potions; stick clicks W / Y (both =
// recentre); left trackpad/view: inventory (hold: map); right trackpad/menu: game menu (hold: character).
constexpr double kTapMs = 90;
constexpr double kHoldMs = 500;

struct Layout {
  double tapUntil[256] = {};
  // stick clicks (W / Y, both together = recentre)
  bool lThumb = false, rThumb = false, chord = false;
  double lThumbAt = 0, rThumbAt = 0;
  // right stick flicks
  bool flickArmed = true;
  // trackpads / menu buttons: tap or hold
  bool backDown = false, startDown = false, backLong = false, startLong = false;
  double backAt = 0, startAt = 0;
};
Layout g_layout;  // render thread only

void Tap(Layout& l, int dik, double now) { l.tapUntil[dik] = now + kTapMs; }

void MapControls(const XINPUT_GAMEPAD& c, double now, Output& out) {
  Layout& l = g_layout;
  auto held = [&](WORD bit) { return (c.wButtons & bit) != 0; };
  out.keys[DIK_1] = held(XINPUT_GAMEPAD_A);
  out.keys[DIK_2] = held(XINPUT_GAMEPAD_B);
  out.keys[DIK_3] = held(XINPUT_GAMEPAD_X);
  out.keys[DIK_4] = held(XINPUT_GAMEPAD_Y);
  out.keys[DIK_R] = held(XINPUT_GAMEPAD_LEFT_SHOULDER);   // health potion
  out.keys[DIK_E] = held(XINPUT_GAMEPAD_RIGHT_SHOULDER);  // energy potion

  // Stick clicks: a quick click = W (left: swap weapons) / Y (right: swap skill bar); both = recentre.
  const bool lt = held(XINPUT_GAMEPAD_LEFT_THUMB), rt = held(XINPUT_GAMEPAD_RIGHT_THUMB);
  if (lt && rt && !l.chord) {
    l.chord = true;
    xr::RequestRecenter();
    Log("pointer: recentre (both sticks)");
  }
  if (lt && !l.lThumb) l.lThumbAt = now;
  if (rt && !l.rThumb) l.rThumbAt = now;
  if (!lt && l.lThumb && !l.chord && now - l.lThumbAt < 400) Tap(l, DIK_W, now);
  if (!rt && l.rThumb && !l.chord && now - l.rThumbAt < 400) Tap(l, DIK_Y, now);
  l.lThumb = lt;
  l.rThumb = rt;
  if (!lt && !rt) l.chord = false;

  // Right stick flicks: hotbar 5 (up), 6 (right), 7 (down), 8 (left).
  const float rx = c.sThumbRX / 32767.0f, ry = c.sThumbRY / 32767.0f;
  const float rmag = std::sqrt(rx * rx + ry * ry);
  if (rmag < 0.35f) l.flickArmed = true;
  if (rmag > 0.75f && l.flickArmed) {
    l.flickArmed = false;
    const int dik = std::fabs(ry) >= std::fabs(rx) ? (ry > 0 ? DIK_5 : DIK_7) : (rx > 0 ? DIK_6 : DIK_8);
    Tap(l, dik, now);
  }

  // Trackpads / menu buttons: tap or hold.
  auto tapOrHold = [&](bool down, bool& wasDown, bool& longFired, double& at, int tapKey, int holdKey) {
    if (down && !wasDown) {
      at = now;
      longFired = false;
    }
    if (down && !longFired && now - at >= kHoldMs) {
      longFired = true;
      Tap(l, holdKey, now);
    }
    if (!down && wasDown && !longFired) Tap(l, tapKey, now);
    wasDown = down;
  };
  tapOrHold(held(XINPUT_GAMEPAD_BACK), l.backDown, l.backLong, l.backAt, DIK_I, DIK_M);
  tapOrHold(held(XINPUT_GAMEPAD_START), l.startDown, l.startLong, l.startAt, DIK_ESCAPE, DIK_C);

  for (int k = 0; k < 256; ++k)
    if (now < l.tapUntil[k]) out.keys[k] = true;
}

double NowMs() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

// A game-space point on the game's own screen (client pixels).
bool ToClient(const GameView& g, Vec3 p, float& x, float& y) {
  const Vec3 d = p - g.origin;
  const float z = Dot(d, g.forward);
  if (z <= 0.01f || g.width <= 0) return false;
  x = (Dot(d, g.right) / (z * g.tanX) * 0.5f + 0.5f) * g.width;
  y = (0.5f - Dot(d, g.up) / (z * g.tanY) * 0.5f) * g.height;
  return std::isfinite(x) && std::isfinite(y);
}

// The game can only click what is on its own (narrow) screen. A spot outside it is pulled in along the
// line from the character until it is just inside, so walking works in every direction.
bool ToClientOnScreen(const GameView& g, Vec3 from, Vec3 to, float& x, float& y, bool& pulled) {
  constexpr float kMargin = 0.04f;  // fraction of the screen kept clear at the edges
  auto inside = [&](Vec3 p, float& px, float& py) {
    if (!ToClient(g, p, px, py) || px < g.width * kMargin || px > g.width * (1 - kMargin) || py < g.height * kMargin ||
        py > g.height * (1 - kMargin))
      return false;
    // Not on a HUD element either (a click there would hit the skill bar instead of the ground).
    return mask::HudAlphaAt(px / g.width, py / g.height) < 0.3f;
  };
  pulled = false;
  if (inside(to, x, y)) return true;
  float px, py;
  if (!inside(from, px, py)) return false;
  float lo = 0, hi = 1;  // fraction of the way from `from` to `to` that is still on screen
  for (int i = 0; i < 20; ++i) {
    const float mid = (lo + hi) * 0.5f;
    float mx, my;
    if (inside(from + (to - from) * mid, mx, my)) lo = mid; else hi = mid;
  }
  pulled = true;
  return ToClient(g, from + (to - from) * lo, x, y);
}

// Projects a game-space point into this eye's image (pixels); false if behind the eye.
bool ToPixels(const numbers::EyeView& v, Vec3 p, float& x, float& y) {
  const Vec3 d = p - v.origin;
  const float z = Dot(d, v.forward);
  if (z <= v.nearPlane) return false;
  x = (Dot(d, v.right) / (z * v.tanX) * 0.5f + 0.5f) * v.width;
  y = (0.5f - Dot(d, v.up) / (z * v.tanY) * 0.5f) * v.height;
  return std::isfinite(x) && std::isfinite(y);
}

bool LiveNow() { return GetTickCount64() - g_lastUpdate.load() < 250; }

}  // namespace

bool Install() {
  g_gameInput = GetModuleHandleW(L"DirectInput.dll");
  g_gameModules[0] = GetModuleHandleW(nullptr);
  g_gameModules[1] = GetModuleHandleW(L"Engine.dll");
  g_gameModules[2] = GetModuleHandleW(L"Game.dll");
  if (!g_gameInput) {
    Log("pointer: DirectInput.dll not loaded (no mouse/keyboard feed)");
    return false;
  }
  void* getCursorPos = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetCursorPos"));
  const bool cursorOk = getCursorPos && MH_CreateHook(getCursorPos, reinterpret_cast<void*>(&HookGetCursorPos),
                                                      reinterpret_cast<void**>(&g_origGetCursorPos)) == MH_OK;
  void *stateA, *dataA, *stateW, *dataW;
  DeviceFunctions(IID_IDirectInput8A, stateA, dataA);
  DeviceFunctions(IID_IDirectInput8W, stateW, dataW);
  bool devicesOk = stateA && dataA;
  g_hookedState[0] = stateA;
  g_hookedState[1] = stateW;
  g_hookedData[0] = dataA;
  g_hookedData[1] = dataW;
  if (void* update = reinterpret_cast<void*>(GetProcAddress(g_gameInput, kUpdate)))
    if (MH_CreateHook(update, reinterpret_cast<void*>(&HookUpdate), reinterpret_cast<void**>(&g_origUpdate)) != MH_OK)
      Log("pointer: could not hook DirectInputDevice::Update");
  if (stateA) devicesOk &= MH_CreateHook(stateA, reinterpret_cast<void*>(&HookStateA), reinterpret_cast<void**>(&g_origStateA)) == MH_OK;
  if (dataA) devicesOk &= MH_CreateHook(dataA, reinterpret_cast<void*>(&HookDataA), reinterpret_cast<void**>(&g_origDataA)) == MH_OK;
  if (stateW && stateW != stateA)
    MH_CreateHook(stateW, reinterpret_cast<void*>(&HookStateW), reinterpret_cast<void**>(&g_origStateW));
  if (dataW && dataW != dataA)
    MH_CreateHook(dataW, reinterpret_cast<void*>(&HookDataW), reinterpret_cast<void**>(&g_origDataW));
  if (void* keyEvent = reinterpret_cast<void*>(GetProcAddress(g_gameInput, kGetKeyEvent)))
    MH_CreateHook(keyEvent, reinterpret_cast<void*>(&HookGetKeyEvent), reinterpret_cast<void**>(&g_origGetKeyEvent));
  if (void* suspend = reinterpret_cast<void*>(GetProcAddress(g_gameInput, kSuspend)))
    if (MH_CreateHook(suspend, reinterpret_cast<void*>(&HookSuspend), reinterpret_cast<void**>(&g_origSuspend)) != MH_OK)
      Log("pointer: could not hook DirectInputDevice::Suspend");
  if (HMODULE game = GetModuleHandleW(L"Game.dll")) {
    void* action = reinterpret_cast<void*>(GetProcAddress(game, kHandleActionFromMouse));
    if (!action || MH_CreateHook(action, reinterpret_cast<void*>(&HookHandleAction),
                                 reinterpret_cast<void**>(&g_origHandleAction)) != MH_OK)
      Log("pointer: could not hook HandleActionFromMouse (diagnostic)");
  }
  Log("pointer: mouse/keyboard feed: cursor %s, devices %s (A/W %s)", cursorOk ? "ok" : "FAILED",
      devicesOk ? "ok" : "FAILED", stateA == stateW ? "shared" : "separate");
  return cursorOk && devicesOk;
}

bool Live() { return LiveNow(); }

void ParkCursor(double ms) { g_parkUntil = NowMs() + ms; }

void TapKey(int dik) { g_forceUntil[dik & 0xFF] = HookNowMs() + 120.0; }

void Update(Vec3 origin, Vec3 right, Vec3 up, Vec3 fwd, float scale, Vec3 target, bool haveTarget,
            const GameView& game) {
  g_lastUpdate = GetTickCount64();
  const xr::Settings& s = xr::GetSettings();
  const double now = NowMs();
  State st;
  Pose aim;
  if (s.pointer && !s.pureController && xr::HandAim(s.pointerHand, aim)) {
    auto toGame = [&](Vec3 rel) { return right * rel.x + up * rel.y + fwd * -rel.z; };  // headset -> game axes
    const Vec3 dirRel = Normalize(Rotate(aim.orientation, {0, 0, -1}));
    st.start = origin + toGame(aim.position) * scale;
    const Vec3 dir = Normalize(toGame(dirRel));
    st.valid = true;
    st.end = st.start + dir * kMaxLength;

    // Ground: the horizontal plane at the character's feet.
    float groundT = -1;
    if (haveTarget && dir.y < -1e-3f) {
      groundT = (target.y - st.start.y) / dir.y;
      if (groundT > 0) {
        Vec3 hit = st.start + dir * groundT;
        const Vec3 flat{hit.x - target.x, 0, hit.z - target.z};
        const float reach = std::sqrt(Dot(flat, flat));
        if (reach > kMaxReach) hit = target + flat * (kMaxReach / reach);
        st.end = hit;
        st.onGround = true;
      }
    }
    // Pointing up (no ground hit): use the ground spot straight ahead at the edge of reach.
    if (!st.onGround && haveTarget) {
      const Vec3 flat{dir.x, 0, dir.z};
      const float len = std::sqrt(Dot(flat, flat));
      if (len > 1e-3f) {
        st.end = target + flat * (kMaxReach * 0.5f / len);
        st.onGround = true;
      }
    }
    // HUD panel: only where it shows something (and never the minimap), in front of the ground hit
    // (both measured along the ray; the panel in metres).
    float u, v, panelDist;
    const bool menu = xr::HudMenuOpen();
    if (xr::HudPanelRay(aim.position, dirRel, u, v, panelDist) && xr::HudSolidAt(u, v, g_onPanelLast) &&
        (menu || groundT <= 0 || panelDist * scale < groundT)) {
      st.onPanel = true;
      st.onGround = false;
      st.panelU = u;
      st.panelV = v;
      st.end = st.start + dir * (panelDist * scale);
    }
    if (st.onPanel) {
      // Steadied: small jitter (hand tremor magnified on the panel) is smoothed out, real moves follow.
      const float rx = st.panelU * game.width, ry = st.panelV * game.height;
      if (!g_onPanelLast) {
        g_smoothX = rx;
        g_smoothY = ry;
      } else {
        const float dx = rx - g_smoothX, dy = ry - g_smoothY;
        const float k = std::sqrt(dx * dx + dy * dy) < 14.0f ? 0.2f : 0.7f;
        g_smoothX += dx * k;
        g_smoothY += dy * k;
      }
      st.haveClient = true;
      st.clientX = g_smoothX;
      st.clientY = g_smoothY;
    } else if (st.onGround) {
      bool pulled = false;
      st.haveClient = haveTarget ? ToClientOnScreen(game, target, st.end, st.clientX, st.clientY, pulled)
                                 : ToClient(game, st.end, st.clientX, st.clientY);
      st.pulled = pulled;
    }
  }

  g_onPanelLast = st.onPanel;

  // Pointer mode: the controls become mouse and keyboard.
  XINPUT_GAMEPAD c{};
  const bool active = input::PointerControls(c);
  Output out;
  float feedX = st.clientX, feedY = st.clientY;
  bool feed = st.haveClient;
  if (active) {
    MapControls(c, now, out);
    const bool clickLaser = c.bRightTrigger > 128;
    // Click lock: right after the trigger is pulled on the HUD the cursor holds still, so the click
    // lands on the button that was pointed at.
    if (clickLaser && !g_clickWasDown && st.onPanel) {
      g_lockUntil = now + 200.0;
      g_lockX = st.clientX;
      g_lockY = st.clientY;
    }
    g_clickWasDown = clickLaser;
    if (now < g_lockUntil && st.onPanel) {
      feedX = g_lockX;
      feedY = g_lockY;
    }
    out.buttons[1] = c.bLeftTrigger > 128;  // right click (skill) at the cursor
    const float sx = c.sThumbLX / 32767.0f, sy = c.sThumbLY / 32767.0f;
    if (clickLaser) {
      out.buttons[0] = true;  // left click at the laser
    } else if (std::sqrt(sx * sx + sy * sy) > 0.25f && haveTarget) {
      // Walk: hold a click a little ahead of the character, in the stick's direction (relative to
      // where the view faces).
      Vec3 dir = right * sx + Normalize(Vec3{fwd.x, 0, fwd.z}) * sy;
      dir = Normalize(Vec3{dir.x, 0, dir.z});
      bool pulled = false;
      if (ToClientOnScreen(game, target, target + dir * kMoveAhead, feedX, feedY, pulled)) {
        feed = true;
        st.moving = true;
        out.buttons[0] = true;
      }
    }
  }
  if (now < g_parkUntil.load()) {  // parked: off-screen, no clicks (screen captures without hover text)
    feed = true;
    feedX = feedY = -4000.0f;
    out.buttons[0] = out.buttons[1] = false;
  }
  if (feed) {
    const long x = std::lround(feedX), y = std::lround(feedY);
    g_motionX += x - g_feedX.exchange(x);  // reported as mouse movement so the game sees a mouse
    g_motionY += y - g_feedY.exchange(y);
  }
  g_feeding = feed;
  if (!active) g_motionX = g_motionY = 0;
  {
    std::lock_guard lock(g_outMutex);
    // Log every key / button change (which controls reached the game as what).
    for (int k = 0; k < 256; ++k)
      if (out.keys[k] != g_out.keys[k]) Log("pointer: key 0x%02X %s", k, out.keys[k] ? "down" : "up");
    for (int b = 0; b < 2; ++b)
      if (out.buttons[b] != g_out.buttons[b])
        Log("pointer: %s button %s at (%.0f, %.0f)%s", b ? "right" : "left", out.buttons[b] ? "down" : "up", feedX, feedY,
            st.moving ? " (stick walk)" : st.pulled ? " (pulled onto the game screen)" : "");
    g_out = out;
  }
  g_active = active;

  // Window client size vs the game's image (a mismatch would offset the pointer in menus).
  static ULONGLONG lastLog = 0;
  if (GetTickCount64() - lastLog > 10000) {
    lastLog = GetTickCount64();
    RECT rc{};
    if (HWND w = overlay::GameWindow()) GetClientRect(w, &rc);
    Log("pointer: mode %s, window client %ldx%ld, game image %.0fx%.0f, %llu input events sent, %llu game mouse actions",
        active ? "mouse+keyboard" : "gamepad/off", rc.right - rc.left, rc.bottom - rc.top, game.width, game.height,
        g_eventsSent.load(), g_actions.load());
    std::string callers;
    for (const CursorCaller& c : g_cursorCallers) {
      HMODULE m = c.mod.load();
      if (!m) continue;
      wchar_t name[MAX_PATH] = L"?";
      GetModuleFileNameW(m, name, MAX_PATH);
      const wchar_t* base = wcsrchr(name, L'\\');
      char line[160];
      snprintf(line, sizeof(line), " %ls %llu (fed %llu);", base ? base + 1 : name, c.calls.load(), c.fed.load());
      callers += line;
    }
    Log("pointer: cursor read by%s", callers.c_str());
  }
  std::lock_guard lock(g_mutex);
  g_state = st;
}

void DrawEye(const numbers::EyeView& v) {
  State st;
  {
    std::lock_guard lock(g_mutex);
    st = g_state;
  }
  if (!st.valid) return;

  // Laser: sampled along its length so the part behind the eye can be dropped.
  std::vector<float> laser;
  constexpr int kSamples = 32;
  for (int i = 0; i <= kSamples; ++i) {
    const float t = float(i) / kSamples;
    float x, y;
    if (ToPixels(v, st.start + (st.end - st.start) * t, x, y)) {
      laser.push_back(x);
      laser.push_back(y);
    }
  }
  // Cursor ring on the ground (a circle in the ground plane, so it looks flat on the floor).
  std::vector<float> ring;
  if (st.onGround) {
    constexpr int kRing = 28;
    for (int i = 0; i < kRing; ++i) {
      const float a = 6.2831853f * float(i) / kRing;
      float x, y;
      if (ToPixels(v, st.end + Vec3{std::cos(a) * kRingRadius, 0.05f, std::sin(a) * kRingRadius}, x, y)) {
        ring.push_back(x);
        ring.push_back(y);
      }
    }
  }
  overlay::Polyline lines[2];
  int n = 0;
  if (laser.size() >= 4)
    lines[n++] = {laser.data(), int(laser.size() / 2), IM_COL32(90, 210, 255, 170), 3.0f, false};
  if (ring.size() >= 6) lines[n++] = {ring.data(), int(ring.size() / 2), IM_COL32(255, 255, 255, 230), 4.0f, true};
  overlay::DrawWorld(nullptr, 0, lines, n);
}

void DrawDebugUI() {
  State st;
  {
    std::lock_guard lock(g_mutex);
    st = g_state;
  }
  Output out;
  {
    std::lock_guard lock(g_outMutex);
    out = g_out;
  }
  std::string keys;
  for (int k = 0; k < 256; ++k)
    if (out.keys[k]) keys += std::to_string(k) + " ";
  ImGui::Text("Pointer: %s, laser %s%s, cursor %s (%ld, %ld)%s", g_active ? "mouse+keyboard mode" : "gamepad/off",
              st.valid ? "tracking" : "off", st.onPanel ? " on HUD" : st.onGround ? " on ground" : "",
              g_feeding ? "fed" : "not fed", g_feedX.load(), g_feedY.load(), st.moving ? " (walking)" : "");
  ImGui::Text("  buttons L %d R %d, keys [%s], %llu events sent", int(out.buttons[0]), int(out.buttons[1]), keys.c_str(),
              g_eventsSent.load());
}

int InFlight() { return g_inFlight.load(); }

}  // namespace gdvr::pointer
