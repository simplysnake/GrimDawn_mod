#include "numbers.h"

#include <MinHook.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "log.h"
#include "overlay.h"
#include "xr.h"

namespace gdvr::numbers {

namespace {

// void EventManager::Send(const GameEvent*, unsigned type)
constexpr char kEventManagerSend[] = "?Send@EventManager@GAME@@QEAAXPEBUGameEvent@2@I@Z";
using SendFn = void (*)(void* manager, const void* event, unsigned type);
SendFn g_origSend = nullptr;

// The combat-text event, as CombatManager::TakeAttack builds it (Game.dll 0x11FA3D..0x11FA7C):
//   +0x08 WorldVec3 position (Region*, then x, y, z)   +0x40 std::wstring text
//   +0x60 style (0x85 crit, 0x46 hit)                  +0x64 size factor (Player::RegisterCombatText*)
constexpr unsigned kCombatTextEvent = 0x1B;
constexpr int kPosOffset = 0x10, kTextOffset = 0x40, kStyleOffset = 0x60, kScaleOffset = 0x64;
constexpr unsigned kStyleCrit = 0x85;

constexpr double kLifeMs = 1200.0;   // how long a number stays
constexpr float kRise = 1.6f;        // game units it floats up over its life
constexpr float kHeight = 0.585f;    // text height in game units at size factor 1
constexpr size_t kMaxNumbers = 96;

struct Number {
  Vec3 pos;
  char text[48];  // UTF-8
  bool crit;
  float scale;
  float drift;    // small sideways offset so numbers from one hit don't stack exactly
  double bornMs;
};

std::atomic<int> g_inFlight{0};
std::mutex g_mutex;
std::vector<Number> g_numbers;
std::atomic<unsigned long long> g_taken{0}, g_unreadable{0};
std::atomic<ULONGLONG> g_lastStereoTick{0};

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

bool StereoOn() { return GetTickCount64() - g_lastStereoTick.load() < 250; }

// Reads the event without trusting it: any fault means "not a combat-text event we understand".
bool ReadEvent(const unsigned char* e, float pos[3], wchar_t* text, size_t textMax, unsigned& style, float& scale) {
  __try {
    std::memcpy(pos, e + kPosOffset, 3 * sizeof(float));
    const unsigned char* str = e + kTextOffset;  // MSVC std::wstring: buffer/pointer, size, capacity
    const size_t size = *reinterpret_cast<const size_t*>(str + 0x10);
    const size_t cap = *reinterpret_cast<const size_t*>(str + 0x18);
    if (size == 0 || size > 64 || cap < size) return false;
    const wchar_t* chars = cap >= 8 ? *reinterpret_cast<wchar_t* const*>(str) : reinterpret_cast<const wchar_t*>(str);
    const size_t n = std::min(size, textMax - 1);
    std::memcpy(text, chars, n * sizeof(wchar_t));
    text[n] = 0;
    style = *reinterpret_cast<const unsigned*>(e + kStyleOffset);
    scale = *reinterpret_cast<const float*>(e + kScaleOffset);
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void HookSend(void* manager, const void* event, unsigned type) {
  ++g_inFlight;
  bool swallow = false;
  if (type == kCombatTextEvent && event && xr::GetSettings().damageNumbers3d && StereoOn()) {
    float pos[3];
    wchar_t wide[48];
    unsigned style = 0;
    float scale = 1;
    if (ReadEvent(static_cast<const unsigned char*>(event), pos, wide, 48, style, scale)) {
      Number n{};
      n.pos = {pos[0], pos[1], pos[2]};
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, n.text, sizeof(n.text), nullptr, nullptr);
      n.crit = style == kStyleCrit;
      n.scale = std::isfinite(scale) ? std::clamp(scale, 0.6f, 2.0f) : 1.0f;
      static unsigned seed = 1;
      seed = seed * 1103515245u + 12345u;
      n.drift = (float((seed >> 16) & 0xFF) / 255.0f - 0.5f) * 0.8f;
      n.bornMs = NowMs();
      {
        std::lock_guard lock(g_mutex);
        if (g_numbers.size() >= kMaxNumbers) g_numbers.erase(g_numbers.begin());
        g_numbers.push_back(n);
      }
      ++g_taken;
      swallow = true;  // drawn in 3D instead of by the game's flat UI
    } else {
      ++g_unreadable;
    }
  }
  if (!swallow) g_origSend(manager, event, type);
  --g_inFlight;
}

}  // namespace

bool Install(HMODULE engine) {
  void* target = reinterpret_cast<void*>(GetProcAddress(engine, kEventManagerSend));
  if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&HookSend), reinterpret_cast<void**>(&g_origSend)) != MH_OK) {
    Log("numbers: could not hook EventManager::Send (3D damage numbers off)");
    return false;
  }
  Log("numbers: EventManager::Send hooked");
  return true;
}

void OnStereoFrame() { g_lastStereoTick = GetTickCount64(); }

void DrawEye(const EyeView& v) {
  const double now = NowMs();
  std::vector<Number> live;  // copies: the text pointers below stay valid until the draw
  {
    std::lock_guard lock(g_mutex);
    g_numbers.erase(std::remove_if(g_numbers.begin(), g_numbers.end(),
                                   [&](const Number& n) { return now - n.bornMs > kLifeMs; }),
                    g_numbers.end());
    live = g_numbers;
  }
  if (live.empty()) return;
  std::vector<overlay::WorldText> items;
  items.reserve(live.size());
  for (const Number& n : live) {
    const float t = float((now - n.bornMs) / kLifeMs);  // 0..1 over its life
    // Floats up (easing out) from where the hit landed, as seen by this eye.
    const Vec3 p = n.pos + Vec3{0, kRise * (1 - (1 - t) * (1 - t)), 0} + v.right * n.drift;
    const Vec3 d = p - v.origin;
    const float z = Dot(d, v.forward);
    if (z <= v.nearPlane) continue;
    const float ndcX = Dot(d, v.right) / (z * v.tanX);
    const float ndcY = Dot(d, v.up) / (z * v.tanY);
    if (std::fabs(ndcX) > 1.3f || std::fabs(ndcY) > 1.3f) continue;
    // Constant size in the world: closer numbers are bigger, like the monsters.
    const float worldHeight = kHeight * n.scale * (n.crit ? 1.35f : 1.0f);
    const float size = std::clamp(worldHeight / (z * v.tanY) * v.height * 0.5f, 10.0f, 160.0f);
    const float fade = t < 0.7f ? 1.0f : 1.0f - (t - 0.7f) / 0.3f;
    const unsigned alpha = unsigned(std::clamp(fade, 0.0f, 1.0f) * 255.0f);
    const unsigned rgb = n.crit ? IM_COL32(255, 196, 40, 0) : IM_COL32(255, 255, 255, 0);
    items.push_back({(ndcX * 0.5f + 0.5f) * v.width, (0.5f - ndcY * 0.5f) * v.height, size, rgb | (alpha << 24),
                     n.text, false});
  }
  overlay::DrawWorldText(items.data(), int(items.size()));
}

void DrawDebugUI() {
  std::lock_guard lock(g_mutex);
  ImGui::Text("3D damage numbers: %zu live, %llu taken from the game, %llu unreadable events", g_numbers.size(),
              g_taken.load(), g_unreadable.load());
}

int InFlight() { return g_inFlight.load(); }

}  // namespace gdvr::numbers
