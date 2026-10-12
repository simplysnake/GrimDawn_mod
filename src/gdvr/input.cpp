#include "input.h"

#include <Xinput.h>

#include <MinHook.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"

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
  kRecenter,
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
    {"recenter", "Recenter view", XR_ACTION_TYPE_BOOLEAN_INPUT},
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
    {kLB, "/user/hand/left/input/squeeze/value"},
    {kRB, "/user/hand/right/input/squeeze/value"},
    {kLT, "/user/hand/left/input/trigger/value"},
    {kRT, "/user/hand/right/input/trigger/value"},
    {kLStickClick, "/user/hand/left/input/thumbstick/click"},
    {kRStickClick, "/user/hand/right/input/thumbstick/click"},
    {kStart, "/user/hand/right/input/trackpad/force"},
    {kBack, "/user/hand/left/input/trackpad/force"},
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
};

const Binding kSimpleBindings[] = {
    {kA, "/user/hand/right/input/select/click"},
    {kX, "/user/hand/left/input/select/click"},
    {kStart, "/user/hand/right/input/menu/click"},
    {kBack, "/user/hand/left/input/menu/click"},
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
    {"/interaction_profiles/khr/simple_controller", kSimpleBindings, std::size(kSimpleBindings)},
};

XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction g_actions[kActionCount] = {};
std::string g_profiles = "none";

// ---- Virtual gamepad ---------------------------------------------------------------------
std::mutex g_padMutex;
XINPUT_GAMEPAD g_pad = {};
bool g_padActive = false;     // some VR controller action is bound and active
DWORD g_padPacket = 0;
bool g_lastRecenter = false;

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

}  // namespace

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
  Log("input: %d actions attached", int(kActionCount));
  return true;
}

bool Sync(XrSession session, bool focused) {
  if (!g_actionSet) return false;
  XINPUT_GAMEPAD pad = {};
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
          {kStart, XINPUT_GAMEPAD_START}, {kBack, XINPUT_GAMEPAD_BACK},
          {kDpadUp, XINPUT_GAMEPAD_DPAD_UP}, {kDpadDown, XINPUT_GAMEPAD_DPAD_DOWN},
          {kDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT}, {kDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT},
      };
      for (const auto& b : buttons)
        if (getBool(b.id)) pad.wButtons |= b.bit;

      const bool r = getBool(kRecenter);
      recenter = r && !g_lastRecenter;
      g_lastRecenter = r;
    }
  }

  std::lock_guard lock(g_padMutex);
  if (std::memcmp(&pad, &g_pad, sizeof(pad)) != 0) ++g_padPacket;
  g_pad = pad;
  g_padActive = anyActive;
  return recenter;
}

void DestroyActions() {
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
  if (!ImGui::CollapsingHeader("Input", ImGuiTreeNodeFlags_DefaultOpen)) return;
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
