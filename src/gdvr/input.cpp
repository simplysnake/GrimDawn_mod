#include "input.h"

#include <Xinput.h>

#include <MinHook.h>
#include <intrin.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "pointer.h"
#include "xr.h"

namespace gdvr::input {

namespace {

// ---- Actions -------------------------------------------------------------------------------
// One action per gamepad control. Names show up in SteamVR's binding UI, so they describe
// what the control does in Grim Dawn's gamepad mode as well as which pad button it is.
enum ActionId {
  kMove, kRightStick,
  kA, kB, kX, kY,
  kLB, kRB, kLT, kRT,
  kLStickClick, kRStickClick,
  kStart, kBack,
  kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
  kLeftPad, kRightPad, kLeftPadTouch, kRightPadTouch,
  kRecenter,
  kAimPose,
  kActionCount
};

struct ActionDef {
  const char* name;       // lowercase, [a-z0-9_-]
  const char* localized;  // shown to the user
  XrActionType type;
};

const ActionDef kActions[kActionCount] = {
    {"move", "Move (left stick)", XR_ACTION_TYPE_VECTOR2F_INPUT},
    {"right_stick", "Right stick", XR_ACTION_TYPE_VECTOR2F_INPUT},
    {"button_a", "A button", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"button_b", "B button", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"button_x", "X button", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"button_y", "Y button", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"left_bumper", "Left bumper", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"right_bumper", "Right bumper", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"left_trigger", "Left trigger", XR_ACTION_TYPE_FLOAT_INPUT},
    {"right_trigger", "Right trigger", XR_ACTION_TYPE_FLOAT_INPUT},
    {"left_stick_click", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"right_stick_click", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"start", "Start / menu", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"back", "Back / view", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"dpad_up", "D-pad up", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"dpad_down", "D-pad down", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"dpad_left", "D-pad left", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"dpad_right", "D-pad right", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"left_pad", "Left touch pad (slide: turn camera)", XR_ACTION_TYPE_VECTOR2F_INPUT},
    {"right_pad", "Right touch pad (slide: zoom)", XR_ACTION_TYPE_VECTOR2F_INPUT},
    {"left_pad_touch", "Left touch pad touched", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"right_pad_touch", "Right touch pad touched", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"recenter", "Recenter view", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {"aim", "Pointer (laser aim)", XR_ACTION_TYPE_POSE_INPUT},
};

struct Binding {
  ActionId action;
  const char* path;
};

// Default bindings per controller type. Everything can be remapped in SteamVR.
const Binding kIndexBindings[] = {
    {kMove, "/user/hand/left/input/thumbstick"},
    {kRightStick, "/user/hand/right/input/thumbstick"},
    {kA, "/user/hand/right/input/a/click"},
    {kB, "/user/hand/right/input/b/click"},
    {kX, "/user/hand/left/input/a/click"},
    {kY, "/user/hand/left/input/b/click"},
    // Index grip "value" reads high just from holding the controller; use squeeze force instead.
    {kLB, "/user/hand/left/input/squeeze/force"},
    {kRB, "/user/hand/right/input/squeeze/force"},
    {kLT, "/user/hand/left/input/trigger/value"},
    {kRT, "/user/hand/right/input/trigger/value"},
    {kLStickClick, "/user/hand/left/input/thumbstick/click"},
    {kRStickClick, "/user/hand/right/input/thumbstick/click"},
    {kStart, "/user/hand/right/input/trackpad/force"},
    {kBack, "/user/hand/left/input/trackpad/force"},
    {kLeftPad, "/user/hand/left/input/trackpad"},
    {kRightPad, "/user/hand/right/input/trackpad"},
    {kLeftPadTouch, "/user/hand/left/input/trackpad/touch"},
    {kRightPadTouch, "/user/hand/right/input/trackpad/touch"},
    {kAimPose, "/user/hand/left/input/aim/pose"},
    {kAimPose, "/user/hand/right/input/aim/pose"},
};

const Binding kTouchBindings[] = {
    {kMove, "/user/hand/left/input/thumbstick"},
    {kRightStick, "/user/hand/right/input/thumbstick"},
    {kA, "/user/hand/right/input/a/click"},
    {kB, "/user/hand/right/input/b/click"},
    {kX, "/user/hand/left/input/x/click"},
    {kY, "/user/hand/left/input/y/click"},
    {kLB, "/user/hand/left/input/squeeze/value"},
    {kRB, "/user/hand/right/input/squeeze/value"},
    {kLT, "/user/hand/left/input/trigger/value"},
    {kRT, "/user/hand/right/input/trigger/value"},
    {kLStickClick, "/user/hand/left/input/thumbstick/click"},
    {kRStickClick, "/user/hand/right/input/thumbstick/click"},
    {kStart, "/user/hand/left/input/menu/click"},
    {kAimPose, "/user/hand/left/input/aim/pose"},
    {kAimPose, "/user/hand/right/input/aim/pose"},
};

const Binding kViveBindings[] = {
    {kMove, "/user/hand/left/input/trackpad"},
    {kRightStick, "/user/hand/right/input/trackpad"},
    {kA, "/user/hand/right/input/trackpad/click"},
    {kX, "/user/hand/left/input/trackpad/click"},
    {kLB, "/user/hand/left/input/squeeze/click"},
    {kRB, "/user/hand/right/input/squeeze/click"},
    {kLT, "/user/hand/left/input/trigger/value"},
    {kRT, "/user/hand/right/input/trigger/value"},
    {kStart, "/user/hand/right/input/menu/click"},
    {kBack, "/user/hand/left/input/menu/click"},
    {kAimPose, "/user/hand/left/input/aim/pose"},
    {kAimPose, "/user/hand/right/input/aim/pose"},
};

const Binding kWmrBindings[] = {
    {kMove, "/user/hand/left/input/thumbstick"},
    {kRightStick, "/user/hand/right/input/thumbstick"},
    {kA, "/user/hand/right/input/trackpad/click"},
    {kX, "/user/hand/left/input/trackpad/click"},
    {kLB, "/user/hand/left/input/squeeze/click"},
    {kRB, "/user/hand/right/input/squeeze/click"},
    {kLT, "/user/hand/left/input/trigger/value"},
    {kRT, "/user/hand/right/input/trigger/value"},
    {kLStickClick, "/user/hand/left/input/thumbstick/click"},
    {kRStickClick, "/user/hand/right/input/thumbstick/click"},
    {kStart, "/user/hand/right/input/menu/click"},
    {kBack, "/user/hand/left/input/menu/click"},
    {kAimPose, "/user/hand/left/input/aim/pose"},
    {kAimPose, "/user/hand/right/input/aim/pose"},
};
const Binding kSimpleBindings[] = {
    {kA, "/user/hand/right/input/select/click"},
    {kX, "/user/hand/left/input/select/click"},
    {kStart, "/user/hand/right/input/menu/click"},
    {kBack, "/user/hand/left/input/menu/click"},
    {kAimPose, "/user/hand/left/input/aim/pose"},
    {kAimPose, "/user/hand/right/input/aim/pose"},
};

struct Profile {
  const char* path;
  const Binding* bindings;
  size_t count;
};

const Profile kProfiles[] = {
    {"/interaction_profiles/valve/index_controller", kIndexBindings, std::size(kIndexBindings)},
    {"/interaction_profiles/oculus/touch_controller", kTouchBindings, std::size(kTouchBindings)},
    {"/interaction_profiles/htc/vive_controller", kViveBindings, std::size(kViveBindings)},
    {"/interaction_profiles/microsoft/motion_controller", kWmrBindings, std::size(kWmrBindings)},
    {"/interaction_profiles/khr/simple_controller", kSimpleBindings, std::size(kSimpleBindings)},
};

XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction g_actions[kActionCount] = {};
XINPUT_GAMEPAD g_vrControls = {};  // VR controls as a pad (under g_padMutex)
TouchPads g_pads;                  // the touch pads (under g_padMutex)
bool g_pointerMode = false;
XrPath g_handPaths[2] = {};
XrSpace g_aimSpaces[2] = {};  // controller aim pose (pointer ray) per hand
std::string g_profiles = "none";

// ---- Virtual gamepad ---------------------------------------------------------------------
std::mutex g_padMutex;
XINPUT_GAMEPAD g_pad = {};
bool g_padActive = false;     // some VR controller action is bound and active
DWORD g_padPacket = 0;
bool g_lastRecenter = false;

// Pressing both stick clicks together recentres the view. Stick clicks are held back briefly so
// that the first stick of the chord does not reach the game on its own.
constexpr ULONGLONG kChordWindowMs = 100;
constexpr int kTapPulseFrames = 3;   // a quick tap is replayed to the game for this many frames
struct StickClick {
  bool down = false;
  ULONGLONG downAt = 0;
  int pulse = 0;
};
StickClick g_clickL, g_clickR;
bool g_chordActive = false;

// Updates one stick click; returns whether the game should see it pressed.
bool GateClick(StickClick& c, bool raw, ULONGLONG now) {
  if (raw && !c.down) {
    c.down = true;
    c.downAt = now;
  } else if (!raw && c.down) {
    c.down = false;
    if (now - c.downAt < kChordWindowMs && !g_chordActive) c.pulse = kTapPulseFrames;
  }
  bool out = raw && now - c.downAt >= kChordWindowMs && !g_chordActive;
  if (c.pulse > 0) {
    --c.pulse;
    out = true;
  }
  return out;
}

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_origGetState = nullptr;
std::atomic<int> g_inFlight{0};
std::atomic<int> g_realPadSeen{0};

SHORT ToStick(float v) {
  v = std::fmax(-1.0f, std::fmin(1.0f, v));
  return SHORT(std::lround(v * 32767.0f));
}
BYTE ToTrigger(float v) { return BYTE(std::lround(std::fmax(0.0f, std::fmin(1.0f, v)) * 255.0f)); }

// Keeps whichever stick is pushed further.
void MergeStick(SHORT& x, SHORT& y, SHORT vx, SHORT vy) {
  const double real = double(x) * x + double(y) * y;
  const double virt = double(vx) * vx + double(vy) * vy;
  if (virt > real) {
    x = vx;
    y = vy;
  }
}

DWORD WINAPI HookXInputGetState(DWORD userIndex, XINPUT_STATE* state) {
  ++g_inFlight;
  DWORD result = g_origGetState(userIndex, state);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) Log("input: game polled XInputGetState (user %lu, result %lu)", userIndex, result);
  if (userIndex == 0 && state) {
    XINPUT_GAMEPAD pad;
    bool active;
    DWORD packet;
    {
      std::lock_guard lock(g_padMutex);
      pad = g_pad;
      active = g_padActive;
      packet = g_padPacket;
    }
    if (result == ERROR_SUCCESS) g_realPadSeen = 1;
    // Diagnostic: when anything is moving or pressed, log real and virtual pad (at most every 300 ms).
    {
      static ULONGLONG lastLog = 0;
      const XINPUT_GAMEPAD& r = state->Gamepad;
      auto busy = [](const XINPUT_GAMEPAD& g) {
        return g.wButtons || g.bLeftTrigger > 30 || g.bRightTrigger > 30 || std::abs(g.sThumbLX) > 8000 ||
               std::abs(g.sThumbLY) > 8000 || std::abs(g.sThumbRX) > 8000 || std::abs(g.sThumbRY) > 8000;
      };
      const bool realBusy = result == ERROR_SUCCESS && busy(r);
      if ((realBusy || (active && busy(pad))) && GetTickCount64() - lastLog > 300) {
        lastLog = GetTickCount64();
        Log("pad: real(%s) btn %04X L %6d %6d R %6d %6d T %3d %3d | vr(%s) btn %04X L %6d %6d R %6d %6d T %3d %3d",
            result == ERROR_SUCCESS ? "on" : "off", r.wButtons, r.sThumbLX, r.sThumbLY, r.sThumbRX, r.sThumbRY,
            r.bLeftTrigger, r.bRightTrigger, active ? "on" : "off", pad.wButtons, pad.sThumbLX, pad.sThumbLY,
            pad.sThumbRX, pad.sThumbRY, pad.bLeftTrigger, pad.bRightTrigger);
      }
    }
    if (active) {
      if (result != ERROR_SUCCESS) {
        std::memset(state, 0, sizeof(*state));
        result = ERROR_SUCCESS;
      }
      XINPUT_GAMEPAD& g = state->Gamepad;
      g.wButtons |= pad.wButtons;
      g.bLeftTrigger = std::max(g.bLeftTrigger, pad.bLeftTrigger);
      g.bRightTrigger = std::max(g.bRightTrigger, pad.bRightTrigger);
      MergeStick(g.sThumbLX, g.sThumbLY, pad.sThumbLX, pad.sThumbLY);
      MergeStick(g.sThumbRX, g.sThumbRY, pad.sThumbRX, pad.sThumbRY);
      state->dwPacketNumber += packet;  // changes whenever either source changes
    }
    if (result == ERROR_SUCCESS) {
      // Both stick clicks on any controller = recentre; neither click reaches the game.
      WORD& b = state->Gamepad.wButtons;
      const ULONGLONG now = GetTickCount64();
      const bool rawL = b & XINPUT_GAMEPAD_LEFT_THUMB, rawR = b & XINPUT_GAMEPAD_RIGHT_THUMB;
      if (rawL && rawR && !g_chordActive) {
        g_chordActive = true;
        xr::RequestRecenter();
        Log("input: recentre (both sticks)");
      }
      const bool outL = GateClick(g_clickL, rawL, now);
      const bool outR = GateClick(g_clickR, rawR, now);
      if (!rawL && !rawR) g_chordActive = false;
      const WORD gated = WORD((outL ? XINPUT_GAMEPAD_LEFT_THUMB : 0) | (outR ? XINPUT_GAMEPAD_RIGHT_THUMB : 0));
      if ((b & (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB)) != gated) state->dwPacketNumber += 0x10000;
      b = WORD((b & ~(XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB)) | gated);
    }
  }
  --g_inFlight;
  return result;
}

bool Check(XrInstance instance, XrResult r, const char* what) {
  if (XR_SUCCEEDED(r)) return true;
  char buf[XR_MAX_RESULT_STRING_SIZE] = "";
  xrResultToString(instance, r, buf);
  Log("input: %s failed: %s", what, buf);
  return false;
}

XrPath ToPath(XrInstance instance, const char* s) {
  XrPath p = XR_NULL_PATH;
  xrStringToPath(instance, s, &p);
  return p;
}

// ---- Keyboard guard -----------------------------------------------------------------------------
// The game's DirectInput.dll installs a system-wide low-level keyboard hook (WH_KEYBOARD_LL, to
// block the Windows key). Windows sends every key press on the PC through that hook first, so
// whenever the game hangs or sits in its crash reporter the whole keyboard stops working until
// the process is gone. The mod refuses that hook (and removes it if it is already installed).
using SetWindowsHookExAFn = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
using UnhookWindowsHookExFn = BOOL(WINAPI*)(HHOOK);
SetWindowsHookExAFn g_origSetHook = nullptr;
UnhookWindowsHookExFn g_origUnhook = nullptr;
HMODULE g_gameInput = nullptr;
const HHOOK kRefusedHook = reinterpret_cast<HHOOK>(static_cast<uintptr_t>(0x6764767221));  // never a real handle

bool CalledFrom(void* returnAddress, HMODULE module) {
  HMODULE mod = nullptr;
  return module &&
         GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(returnAddress), &mod) &&
         mod == module;
}

HHOOK WINAPI HookSetWindowsHookExA(int id, HOOKPROC proc, HINSTANCE instance, DWORD thread) {
  if (id == WH_KEYBOARD_LL && CalledFrom(_ReturnAddress(), g_gameInput)) {
    Log("input: refused the game's low-level keyboard hook (it freezes the PC keyboard when the game hangs)");
    return kRefusedHook;
  }
  return g_origSetHook(id, proc, instance, thread);
}

BOOL WINAPI HookUnhookWindowsHookEx(HHOOK hook) {
  if (hook == kRefusedHook) return TRUE;
  return g_origUnhook(hook);
}

// The game's controller on-screen keyboard (Grim Dawn.exe+0x104E10 / +0x106B78) types by pressing
// real keys system-wide with SendInput. With VR controllers it ends up sending keys such as the
// Windows key (Index A/B opened the Start menu, took focus away and could leave the key stuck).
// While VR is running the mod's controls reach the game directly, so the game's SendInput is dropped.
using SendInputFn = UINT(WINAPI*)(UINT, LPINPUT, int);
SendInputFn g_origSendInput = nullptr;
std::atomic<int> g_sendInputLogs{0};

UINT WINAPI HookSendInput(UINT count, LPINPUT inputs, int size) {
  ++g_inFlight;
  UINT result;
  if (CalledFrom(_ReturnAddress(), GetModuleHandleW(nullptr)) && pointer::Live()) {
    if (g_sendInputLogs < 50) {
      ++g_sendInputLogs;
      for (UINT i = 0; i < count && inputs && size == int(sizeof(INPUT)); ++i)
        if (inputs[i].type == INPUT_KEYBOARD)
          Log("input: blocked the game's SendInput key: vk 0x%02X scan 0x%02X flags 0x%lX", inputs[i].ki.wVk,
              inputs[i].ki.wScan, inputs[i].ki.dwFlags);
        else
          Log("input: blocked the game's SendInput (type %lu)", inputs[i].type);
    }
    result = count;  // as if sent
  } else {
    result = g_origSendInput(count, inputs, size);
  }
  --g_inFlight;
  return result;
}

}  // namespace

bool InstallKeyboardGuard() {
  g_gameInput = GetModuleHandleW(L"DirectInput.dll");
  if (!g_gameInput) return false;
  // Already installed (the game started before the mod): remove it now.
  auto* installed = reinterpret_cast<HHOOK*>(
      GetProcAddress(g_gameInput, "?keyboardHook@DirectInputDevice@GAME@@0PEAUHHOOK__@@EA"));
  if (installed && *installed && *installed != kRefusedHook) {
    const BOOL removed = UnhookWindowsHookEx(*installed);
    Log("input: removed the game's low-level keyboard hook (%s)", removed ? "ok" : "failed");
    if (removed) *installed = kRefusedHook;  // the game's own later unhook then goes to the stand-in
  }
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  void* set = reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowsHookExA"));
  void* unhook = reinterpret_cast<void*>(GetProcAddress(user32, "UnhookWindowsHookEx"));
  const bool ok =
      set && unhook &&
      MH_CreateHook(set, reinterpret_cast<void*>(&HookSetWindowsHookExA), reinterpret_cast<void**>(&g_origSetHook)) ==
          MH_OK &&
      MH_CreateHook(unhook, reinterpret_cast<void*>(&HookUnhookWindowsHookEx),
                    reinterpret_cast<void**>(&g_origUnhook)) == MH_OK;
  if (void* sendInput = reinterpret_cast<void*>(GetProcAddress(user32, "SendInput")))
    if (MH_CreateHook(sendInput, reinterpret_cast<void*>(&HookSendInput), reinterpret_cast<void**>(&g_origSendInput)) !=
        MH_OK)
      Log("input: could not hook SendInput");
  Log(ok ? "input: keyboard guard on" : "input: keyboard guard could not be installed");
  return ok;
}

bool InstallXInputHook() {
  // The game's DirectInput.dll imports XInputGetState by ordinal (#2) from xinput1_3.dll.
  HMODULE xinput = GetModuleHandleW(L"xinput1_3.dll");
  if (!xinput) {
    Log("input: xinput1_3.dll not loaded; gamepad merge disabled");
    return false;
  }
  void* target = reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState"));
  if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&HookXInputGetState),
                               reinterpret_cast<void**>(&g_origGetState)) != MH_OK) {
    Log("input: failed to hook XInputGetState");
    return false;
  }
  Log("input: XInputGetState hooked");
  return true;
}

bool CreateActions(XrInstance instance, XrSession session) {
  XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy_s(si.actionSetName, "gameplay");
  strcpy_s(si.localizedActionSetName, "Gameplay");
  if (!Check(instance, xrCreateActionSet(instance, &si, &g_actionSet), "xrCreateActionSet")) return false;

  for (int i = 0; i < kActionCount; ++i) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(ai.actionName, kActions[i].name);
    strcpy_s(ai.localizedActionName, kActions[i].localized);
    ai.actionType = kActions[i].type;
    if (ai.actionType == XR_ACTION_TYPE_POSE_INPUT) {  // one pose per hand
      g_handPaths[0] = ToPath(instance, "/user/hand/left");
      g_handPaths[1] = ToPath(instance, "/user/hand/right");
      ai.countSubactionPaths = 2;
      ai.subactionPaths = g_handPaths;
    }
    if (!Check(instance, xrCreateAction(g_actionSet, &ai, &g_actions[i]), kActions[i].name)) return false;
  }

  for (const Profile& profile : kProfiles) {
    std::vector<XrActionSuggestedBinding> bindings;
    for (size_t i = 0; i < profile.count; ++i)
      bindings.push_back({g_actions[profile.bindings[i].action], ToPath(instance, profile.bindings[i].path)});
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = ToPath(instance, profile.path);
    sb.countSuggestedBindings = uint32_t(bindings.size());
    sb.suggestedBindings = bindings.data();
    Check(instance, xrSuggestInteractionProfileBindings(instance, &sb), profile.path);
  }

  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &g_actionSet;
  if (!Check(instance, xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets")) return false;
  for (int hand = 0; hand < 2; ++hand) {
    XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    asi.action = g_actions[kAimPose];
    asi.subactionPath = g_handPaths[hand];
    asi.poseInActionSpace.orientation.w = 1;
    Check(instance, xrCreateActionSpace(session, &asi, &g_aimSpaces[hand]), "xrCreateActionSpace(aim)");
  }
  Log("input: %d actions attached", int(kActionCount));
  return true;
}

bool Sync(XrSession session, bool focused) {
  if (!g_actionSet) return false;
  XINPUT_GAMEPAD pad = {};
  TouchPads pads;
  bool anyActive = false;
  bool recenter = false;

  if (focused) {
    XrActiveActionSet active{g_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_SUCCEEDED(xrSyncActions(session, &sync))) {
      auto getBool = [&](ActionId id) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = g_actions[id];
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(session, &gi, &s)) || !s.isActive) return false;
        anyActive = true;
        return bool(s.currentState);
      };
      auto getFloat = [&](ActionId id) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = g_actions[id];
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(session, &gi, &s)) || !s.isActive) return 0.0f;
        anyActive = true;
        return s.currentState;
      };
      auto getVec = [&](ActionId id) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = g_actions[id];
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(session, &gi, &s)) || !s.isActive) return XrVector2f{0, 0};
        anyActive = true;
        return s.currentState;
      };

      const XrVector2f move = getVec(kMove);
      const XrVector2f right = getVec(kRightStick);
      pad.sThumbLX = ToStick(move.x);
      pad.sThumbLY = ToStick(move.y);
      pad.sThumbRX = ToStick(right.x);
      pad.sThumbRY = ToStick(right.y);
      pad.bLeftTrigger = ToTrigger(getFloat(kLT));
      pad.bRightTrigger = ToTrigger(getFloat(kRT));
      const struct { ActionId id; WORD bit; } buttons[] = {
          {kA, XINPUT_GAMEPAD_A}, {kB, XINPUT_GAMEPAD_B}, {kX, XINPUT_GAMEPAD_X}, {kY, XINPUT_GAMEPAD_Y},
          {kLB, XINPUT_GAMEPAD_LEFT_SHOULDER}, {kRB, XINPUT_GAMEPAD_RIGHT_SHOULDER},
          {kLStickClick, XINPUT_GAMEPAD_LEFT_THUMB}, {kRStickClick, XINPUT_GAMEPAD_RIGHT_THUMB},
          {kDpadUp, XINPUT_GAMEPAD_DPAD_UP}, {kDpadDown, XINPUT_GAMEPAD_DPAD_DOWN},
          {kDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT}, {kDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT},
      };
      for (const auto& b : buttons)
        if (getBool(b.id)) pad.wButtons |= b.bit;
      if (getBool(kStart)) pad.wButtons |= XINPUT_GAMEPAD_START;
      if (getBool(kBack)) pad.wButtons |= XINPUT_GAMEPAD_BACK;
      pads.left = getVec(kLeftPad);
      pads.right = getVec(kRightPad);
      pads.leftTouch = getBool(kLeftPadTouch);
      pads.rightTouch = getBool(kRightPadTouch);
      pads.leftPress = (pad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
      pads.rightPress = (pad.wButtons & XINPUT_GAMEPAD_START) != 0;

      const bool r = getBool(kRecenter);
      recenter = r && !g_lastRecenter;
      g_lastRecenter = r;
    }
  }

  std::lock_guard lock(g_padMutex);
  // Pointer mode: the VR controls drive the game as mouse and keyboard (pointer module), so the
  // virtual gamepad stays silent and the game leaves its gamepad mode. Without stereo frames (main
  // menu, loading) there is no pointer, so the gamepad takes over again.
  g_vrControls = pad;
  g_pads = pads;
  g_pointerMode = anyActive && xr::GetSettings().pointer && !xr::GetSettings().pureController && pointer::Live();
  if (g_pointerMode) {
    pad = {};
    anyActive = false;
  }
  if (std::memcmp(&pad, &g_pad, sizeof(pad)) != 0) ++g_padPacket;
  g_pad = pad;
  g_padActive = anyActive;
  return recenter;
}

TouchPads Pads() {
  std::lock_guard lock(g_padMutex);
  return g_pads;
}

bool PointerControls(XINPUT_GAMEPAD& controls) {
  std::lock_guard lock(g_padMutex);
  controls = g_vrControls;
  return g_pointerMode;
}

bool LocateAim(int hand, XrSpace base, XrTime time, XrPosef& pose) {
  if (!g_aimSpaces[hand & 1] || !base) return false;
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(g_aimSpaces[hand & 1], base, time, &loc))) return false;
  const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
  if ((loc.locationFlags & need) != need) return false;
  pose = loc.pose;
  return true;
}

void DestroyActions() {
  for (XrSpace& sp : g_aimSpaces) {
    if (sp) xrDestroySpace(sp);
    sp = XR_NULL_HANDLE;
  }
  if (g_actionSet) xrDestroyActionSet(g_actionSet);  // also destroys its actions
  g_actionSet = XR_NULL_HANDLE;
  std::memset(g_actions, 0, sizeof(g_actions));
  std::lock_guard lock(g_padMutex);
  g_pad = {};
  g_padActive = false;
  g_profiles = "none";
}

void OnInteractionProfileChanged(XrInstance instance, XrSession session) {
  std::string text;
  for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
    XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
    char buf[XR_MAX_PATH_LENGTH] = "none";
    uint32_t len = 0;
    if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, ToPath(instance, hand), &st)) &&
        st.interactionProfile != XR_NULL_PATH)
      xrPathToString(instance, st.interactionProfile, sizeof(buf), &len, buf);
    text += std::string(hand + 11) + "=" + buf + " ";
  }
  g_profiles = text;
  Log("input: controllers %s", text.c_str());
}

int InFlight() { return g_inFlight.load(); }

void DrawDebugUI() {
  XINPUT_GAMEPAD p;
  bool active;
  {
    std::lock_guard lock(g_padMutex);
    p = g_pad;
    active = g_padActive;
  }
  ImGui::Text("Controllers: %s", g_profiles.c_str());
  ImGui::Text("VR pad active=%d  real gamepad seen=%d  buttons=0x%04X", int(active), g_realPadSeen.load(), p.wButtons);
  ImGui::Text("L %6d %6d  R %6d %6d  LT %3d RT %3d", p.sThumbLX, p.sThumbLY, p.sThumbRX, p.sThumbRY, p.bLeftTrigger,
              p.bRightTrigger);
}

}  // namespace gdvr::input
