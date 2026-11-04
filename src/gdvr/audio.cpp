#include "audio.h"

#include <MinHook.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <mutex>

#include "log.h"
#include "xr.h"

namespace gdvr::audio {

namespace {

// void WorldCamera::SetListenerPosition(): calls SoundManager::SetEarsPosition, then (inlined) stores the
// ears' facing and up in the SoundManager and hands them to the sound system.
constexpr char kSetListener[] = "?SetListenerPosition@WorldCamera@GAME@@QEAAXXZ";
// void SoundManager::SetEarsPosition(const WorldVec3&): only called from SetListenerPosition; gives us the manager.
constexpr char kSetEarsPosition[] = "?SetEarsPosition@SoundManager@GAME@@QEAAXAEBVWorldVec3@2@@Z";
// void SoundManager::SetEarsOrientation(const Vec3& up, const Vec3& facing) (Engine.dll 0x1B70E0: the first
// argument goes where SetListenerPosition stores world up, the second where it stores the facing).
constexpr char kSetEarsOrientation[] = "?SetEarsOrientation@SoundManager@GAME@@QEAAXAEBVVec3@2@0@Z";
// Where the SoundManager keeps them (written by SetListenerPosition, Engine.dll 0x229500 / 0x229528).
constexpr int kFacingOffset = 0xA58, kUpOffset = 0xA64;

using ListenerFn = void (*)(void* camera);
using EarsPositionFn = void (*)(void* manager, const void* worldVec3);
using EarsOrientationFn = void (*)(void* manager, const Vec3* up, const Vec3* facing);
ListenerFn g_origListener = nullptr;
EarsPositionFn g_origEarsPosition = nullptr;
EarsOrientationFn g_setEarsOrientation = nullptr;

std::atomic<int> g_inFlight{0};
std::atomic<void*> g_manager{nullptr};
thread_local bool t_earsSet = false;  // SetEarsPosition ran inside this SetListenerPosition call

std::mutex g_mutex;
Vec3 g_base[3], g_head[3];  // right, up, forward
ULONGLONG g_headTick = 0;

// Status for F8.
std::atomic<unsigned long long> g_turned{0};
std::atomic<bool> g_layoutBad{false};
Vec3 g_lastFacing{}, g_lastGameFacing{};

struct Guard {
  Guard() { ++g_inFlight; }
  ~Guard() { --g_inFlight; }
};

bool ReadEars(void* manager, Vec3& facing, Vec3& up) {
  __try {
    const auto* m = static_cast<const unsigned char*>(manager);
    const auto* f = reinterpret_cast<const float*>(m + kFacingOffset);
    const auto* u = reinterpret_cast<const float*>(m + kUpOffset);
    facing = {f[0], f[1], f[2]};
    up = {u[0], u[1], u[2]};
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// What the game writes: a unit horizontal facing and world up. Anything else means a game patch moved them.
bool LooksLikeGameEars(Vec3 facing, Vec3 up) {
  const float len = std::sqrt(Dot(facing, facing));
  return std::isfinite(len) && std::fabs(len - 1.0f) < 0.01f && std::fabs(facing.y) < 0.01f &&
         std::fabs(up.x) < 0.01f && std::fabs(up.y - 1.0f) < 0.01f && std::fabs(up.z) < 0.01f;
}

void HookEarsPosition(void* manager, const void* worldVec3) {
  Guard guard;
  g_manager = manager;
  t_earsSet = true;
  g_origEarsPosition(manager, worldVec3);
}

void HookListener(void* camera) {
  Guard guard;
  t_earsSet = false;
  g_origListener(camera);
  void* manager = g_manager.load();
  if (!t_earsSet || !manager || !xr::GetSettings().headAudio) return;

  Vec3 base[3], head[3];
  {
    std::lock_guard lock(g_mutex);
    if (GetTickCount64() - g_headTick > 250) return;  // not in stereo
    for (int i = 0; i < 3; ++i) {
      base[i] = g_base[i];
      head[i] = g_head[i];
    }
  }

  Vec3 facing, up;
  if (!ReadEars(manager, facing, up)) return;
  if (!LooksLikeGameEars(facing, up)) {
    if (!g_layoutBad.exchange(true))
      Log("audio: listener layout not recognised (facing %.2f %.2f %.2f, up %.2f %.2f %.2f): head-tracked sound off",
          facing.x, facing.y, facing.z, up.x, up.y, up.z);
    return;
  }

  // The head's rotation relative to the frame it moves in, applied in game space.
  auto turn = [&](Vec3 v) { return head[0] * Dot(v, base[0]) + head[1] * Dot(v, base[1]) + head[2] * Dot(v, base[2]); };
  const Vec3 newFacing = Normalize(turn(facing)), newUp = Normalize(turn(up));
  g_setEarsOrientation(manager, &newUp, &newFacing);
  if (g_turned++ == 0) Log("audio: head-tracked sound on (game facing %.2f %.2f %.2f)", facing.x, facing.y, facing.z);
  g_lastGameFacing = facing;
  g_lastFacing = newFacing;
}

}  // namespace

bool Install(HMODULE engine) {
  void* listener = reinterpret_cast<void*>(GetProcAddress(engine, kSetListener));
  void* earsPosition = reinterpret_cast<void*>(GetProcAddress(engine, kSetEarsPosition));
  g_setEarsOrientation = reinterpret_cast<EarsOrientationFn>(GetProcAddress(engine, kSetEarsOrientation));
  if (!listener || !earsPosition || !g_setEarsOrientation ||
      MH_CreateHook(listener, reinterpret_cast<void*>(&HookListener), reinterpret_cast<void**>(&g_origListener)) != MH_OK ||
      MH_CreateHook(earsPosition, reinterpret_cast<void*>(&HookEarsPosition),
                    reinterpret_cast<void**>(&g_origEarsPosition)) != MH_OK) {
    Log("audio: could not hook the sound listener (head-tracked sound off)");
    return false;
  }
  Log("audio: WorldCamera::SetListenerPosition hooked");
  return true;
}

void SetHeadFrame(Vec3 baseRight, Vec3 baseUp, Vec3 baseForward, Vec3 headRight, Vec3 headUp, Vec3 headForward) {
  std::lock_guard lock(g_mutex);
  g_base[0] = baseRight;
  g_base[1] = baseUp;
  g_base[2] = baseForward;
  g_head[0] = headRight;
  g_head[1] = headUp;
  g_head[2] = headForward;
  g_headTick = GetTickCount64();
}

void DrawDebugUI() {
  if (g_layoutBad) {
    ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Listener layout not recognised (game patch?): off");
    return;
  }
  if (!g_manager.load()) {
    ImGui::TextDisabled("Sound listener not seen yet");
    return;
  }
  ImGui::Text("Ears turned %llu times", g_turned.load());
  ImGui::Text("Game facing %.2f %.2f %.2f -> head %.2f %.2f %.2f", g_lastGameFacing.x, g_lastGameFacing.y,
              g_lastGameFacing.z, g_lastFacing.x, g_lastFacing.y, g_lastFacing.z);
}

int InFlight() { return g_inFlight.load(); }

}  // namespace gdvr::audio
