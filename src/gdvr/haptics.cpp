#include "haptics.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "input.h"
#include "log.h"
#include "xr.h"

namespace gdvr::haptics {

namespace {

constexpr double kDamageWindowMs = 100;   // life lost is added up over this long, then felt as one pulse
constexpr float kDamageMinFraction = 0.004f;  // less than this share of max life (regen jitter, tiny DoTs): no pulse
constexpr float kHealFraction = 0.08f;    // a rise of this share of max life in one frame: a potion or big heal
constexpr double kHitGapMs = 60;          // at most one hit tick this often

std::atomic<int> g_hits{0};
std::atomic<bool> g_crit{false};

// Player life, read through the game's own exported getters.
struct Game {
  void** engine = nullptr;
  void* (*mainPlayer)(void*) = nullptr;
  double (*life)(const void*) = nullptr;      // Character::GetCurrentLife
  float (*lifeLimit)(const void*) = nullptr;  // Character::GetLifeLimit (max life)
  bool resolved = false;
  bool ok = false;
} g_game;

void Resolve() {
  g_game.resolved = true;
  HMODULE game = GetModuleHandleW(L"Game.dll");
  if (!game) return;
  g_game.engine = reinterpret_cast<void**>(GetProcAddress(game, "?gGameEngine@GAME@@3PEAVGameEngine@1@EA"));
  g_game.mainPlayer = reinterpret_cast<void* (*)(void*)>(
      GetProcAddress(game, "?GetMainPlayer@GameEngine@GAME@@QEBAPEAVPlayer@2@XZ"));
  g_game.life = reinterpret_cast<double (*)(const void*)>(GetProcAddress(game, "?GetCurrentLife@Character@GAME@@QEBA?BNXZ"));
  g_game.lifeLimit =
      reinterpret_cast<float (*)(const void*)>(GetProcAddress(game, "?GetLifeLimit@Character@GAME@@QEBA?BMXZ"));
  g_game.ok = g_game.engine && g_game.mainPlayer && g_game.life && g_game.lifeLimit;
  Log("haptics: %s", g_game.ok ? "player life found" : "player life getters not found (hits taken not felt)");
}

// The main player and its life; false on the title screen or if the read faults.
bool ReadLife(void*& player, float& life, float& maxLife) {
  if (!g_game.resolved) Resolve();
  if (!g_game.ok) return false;
  __try {
    if (!*g_game.engine) return false;
    player = g_game.mainPlayer(*g_game.engine);
    if (!player) return false;
    life = float(g_game.life(player));
    maxLife = g_game.lifeLimit(player);
    return std::isfinite(life) && std::isfinite(maxLife) && maxLife > 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_game.ok = false;
    Log("haptics: reading the player's life faulted; hits taken not felt");
    return false;
  }
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

void* g_player = nullptr;
float g_lastLife = -1;
float g_lost = 0;              // life lost since the window started
double g_lostSince = 0;
double g_lastHitTick = 0;
// For the F8 window.
float g_shownLife = 0, g_shownMax = 0;
unsigned g_pulses = 0;

void Pulse(int hand, float amplitude, float seconds) {
  const xr::Settings& s = xr::GetSettings();
  const float a = std::clamp(amplitude * s.hapticsStrength, 0.0f, 1.0f);
  if (a <= 0.01f) return;
  input::Vibrate(hand, a, seconds);
  ++g_pulses;
}

}  // namespace

void OnHit(bool crit) {
  ++g_hits;
  if (crit) g_crit = true;
}

void Update() {
  const xr::Settings& s = xr::GetSettings();
  const int hits = g_hits.exchange(0);
  const bool crit = g_crit.exchange(false);
  if (!s.haptics) {
    g_lastLife = -1;
    return;
  }
  const double now = NowMs();
  const int pointerHand = s.pointerHand & 1, offHand = 1 - pointerHand;

  // Hits dealt: a light tick in the weapon (laser) hand, crits stronger.
  if (s.hapticsHits && hits > 0 && now - g_lastHitTick >= kHitGapMs) {
    g_lastHitTick = now;
    Pulse(pointerHand, crit ? 0.55f : 0.22f, crit ? 0.045f : 0.018f);
  }

  void* player = nullptr;
  float life = 0, maxLife = 0;
  if (!ReadLife(player, life, maxLife)) {
    g_player = nullptr;
    g_lastLife = -1;
    return;
  }
  g_shownLife = life;
  g_shownMax = maxLife;
  if (player != g_player || g_lastLife < 0) {  // new character or area load: start over
    g_player = player;
    g_lastLife = life;
    g_lost = 0;
    g_lostSince = now;
    return;
  }
  const float delta = life - g_lastLife;
  g_lastLife = life;

  // Death: one long pulse in both hands.
  if (life <= 0 && delta < 0) {
    Pulse(0, 1.0f, 0.5f);
    Pulse(1, 1.0f, 0.5f);
    g_lost = 0;
    g_lostSince = now;
    return;
  }
  // Hits taken: life lost is added up over a short window and felt in both hands, harder the bigger the hit.
  if (delta < 0) {
    if (g_lost == 0) g_lostSince = now;
    g_lost += -delta;
  }
  if (g_lost > 0 && now - g_lostSince >= kDamageWindowMs) {
    const float frac = g_lost / maxLife;
    if (frac >= kDamageMinFraction) {
      const float amp = std::clamp(0.3f + frac * 3.5f, 0.3f, 1.0f);
      const float secs = std::clamp(0.035f + frac * 0.8f, 0.035f, 0.25f);
      Pulse(0, amp, secs);
      Pulse(1, amp, secs);
    }
    g_lost = 0;
  }
  // A potion or a big heal: a soft pulse in the off hand.
  if (delta > kHealFraction * maxLife) Pulse(offHand, 0.25f, 0.08f);
}

void DrawDebugUI() {
  xr::Settings& s = xr::GetSettings();
  ImGui::Checkbox("Vibration (hits taken, potions, your hits)", &s.haptics);
  if (!s.haptics) return;
  ImGui::SliderFloat("Vibration strength", &s.hapticsStrength, 0.1f, 1.5f, "%.2f");
  ImGui::Checkbox("Tick on your own hits (stronger on crits)", &s.hapticsHits);
  if (!g_game.ok && g_game.resolved)
    ImGui::TextDisabled("Player life not readable: only your hits are felt.");
  else if (g_shownMax > 0)
    ImGui::TextDisabled("Life %.0f / %.0f, %u pulses so far", g_shownLife, g_shownMax, g_pulses);
}

}  // namespace gdvr::haptics
