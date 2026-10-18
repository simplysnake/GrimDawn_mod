#include "engine_hooks.h"

#include <windows.h>

#include <MinHook.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <cmath>

#include "engine_api.h"
#include "d3dtrace.h"
#include "input.h"
#include "labels.h"
#include "log.h"
#include "mask.h"
#include "numbers.h"
#include "perf.h"
#include "pointer.h"
#include "sky.h"
#include "vr_math.h"
#include "xr.h"

namespace gdvr::engine {

namespace {

constexpr int kMaxCalls = 8;      // render calls per frame we keep stats for
constexpr int kCoordsFloats = 16; // floats shown from Camera::GetCoords (layout not yet known)
constexpr int kCoordsCopy = 32;   // floats copied for the offset experiment (> sizeof(Coords))

struct RenderCall {
  const void* camera = nullptr;
  int vpWidth = 0, vpHeight = 0;
  unsigned flags = 0;
  bool flag2 = false;
  float coords[kCoordsFloats] = {};
  float fov = 0, nearPlane = 0, farPlane = 0, aspect = 0;
  double ms = 0;        // time spent in the original Render
  double extraMs = 0;   // time spent in the duplicate Render (if enabled)
};

struct Frame {
  int count = 0;        // total calls this frame (may exceed kMaxCalls)
  RenderCall calls[kMaxCalls];
};

// Engine functions
gd::RendererRenderFn g_origRender = nullptr;
gd::CameraGetCoordsFn g_getCoords = nullptr;
gd::CameraSetCoordsFn g_setCoords = nullptr;
gd::CameraGetFloatFn g_getFov = nullptr, g_getNear = nullptr, g_getFar = nullptr, g_getAspect = nullptr;
gd::ViewportGetIntFn g_vpWidth = nullptr, g_vpHeight = nullptr;
gd::CameraSetFloatFn g_setFov = nullptr, g_setAspect = nullptr, g_setFar = nullptr;
gd::CameraGetProjectionFn g_getProj = nullptr;
gd::CameraGetFrustumFn g_getFrustum = nullptr;
gd::RendererGetCameraTargetFn g_cameraTarget = nullptr;



std::atomic<int> g_inFlight{0};
std::mutex g_mutex;
Frame g_building;   // filled during the frame (guarded by g_mutex)
Frame g_published;  // last complete frame, read by the UI (guarded by g_mutex)

// Experiment controls (written by the UI, read by the render hook)
struct Experiments {
  bool offsetEnabled = false;
  int originIndex = 9;            // guess: Coords = xAxis, yAxis, zAxis, origin (12 floats)
  float offset[3] = {0, 0, 0};
  int targetCall = 0;             // -1 = every call
  bool renderTwice = false;       // call the original Render a second time (cost/stability test)
};
Experiments g_exp;  // plain struct; torn reads are harmless for a debug tool

// While VR is on, the game must keep running when its window loses focus (SteamVR dashboard,
// clicking another window): report the window as active. Input stays focus-gated in DirectInput.
gd::WinWindowIsActiveFn g_origIsActive = nullptr;

bool HookIsActive(void* self) {
  if (xr::GetSettings().mode != xr::Mode::Off && xr::GetSettings().keepRunningUnfocused) return true;
  return g_origIsActive(self);
}

struct InFlightGuard {
  InFlightGuard() { ++g_inFlight; }
  ~InFlightGuard() { --g_inFlight; }
};


double NowMs() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return 1000.0 * double(t.QuadPart) / double(freq.QuadPart);
}

// Coords layout (confirmed in week 1): side axis, up, forward (unit axes), then position.
// Floats 0-2, 3-5, 6-8, 9-11. The side axis points to screen-LEFT (the engine's projection
// matrix has a negative X scale), so screen-right = -side.
constexpr int kSide = 0, kUp = 3, kForward = 6, kOrigin = 9;
// WorldVec3 (GetCameraTarget) = region pointer (8 bytes), then x, y, z: position at float index 2.
constexpr int kTargetPos = 2;

Vec3 Read3(const float* f, int at) { return {f[at], f[at + 1], f[at + 2]}; }
void Write3(float* f, int at, Vec3 v) {
  f[at] = v.x;
  f[at + 1] = v.y;
  f[at + 2] = v.z;
}

// OpenXR space (right-handed, -Z forward) -> game camera space (left-handed, +Z forward).
Vec3 XrToCamera(Vec3 v) { return {v.x, v.y, -v.z}; }

// The tangents of the half-angles the engine renders with, read from its projection matrix.
void ReadTanFov(const gd::Camera* cam, const gd::Viewport* viewport, float& tanX, float& tanY) {
  float m[16] = {};
  g_getProj(cam, reinterpret_cast<gd::Mat4*>(m), viewport);
  tanX = m[0] != 0 ? 1.0f / std::fabs(m[0]) : 1.0f;
  tanY = m[5] != 0 ? 1.0f / std::fabs(m[5]) : 1.0f;
}

// Sets the camera's FOV (vertical) and aspect so its symmetric frustum just covers the eye's
// lens, then returns what the engine will really use. fovDeg > 0 forces a vertical FOV instead.
void FitFrustum(gd::Camera* cam, const gd::Viewport* viewport, int eye, float fovDeg, float& tanX, float& tanY) {
  constexpr float kMargin = 1.04f;  // a little extra so reprojection doesn't show edges
  if (fovDeg > 0) {
    g_setFov(cam, fovDeg * 3.14159265f / 180.0f);
    ReadTanFov(cam, viewport, tanX, tanY);
    return;
  }
  float wantX, wantY;
  xr::EyeTanFov(eye, wantX, wantY);
  wantX *= kMargin;
  wantY *= kMargin;
  g_setFov(cam, 2.0f * std::atan(wantY));
  g_setAspect(cam, 1.0f);
  float baseX, baseY;
  ReadTanFov(cam, viewport, baseX, baseY);

  // The aspect multiplier stretches the horizontal extent; which way round is learnt once.
  static int direction = 0;  // +1: tanX grows with aspect, -1: shrinks, 2: aspect has no usable effect
  const float ratio = wantX / baseX;
  auto close = [&](float v) { return std::fabs(v - wantX) < 0.03f * wantX; };
  if (direction == 0) {
    for (int d : {+1, -1}) {
      g_setAspect(cam, d > 0 ? ratio : 1.0f / ratio);
      ReadTanFov(cam, viewport, tanX, tanY);
      if (close(tanX)) {
        direction = d;
        break;
      }
    }
    if (direction == 0) direction = 2;
    Log("stereo: lens tan %.3f x %.3f, base %.3f x %.3f, aspect direction %d", wantX, wantY, baseX, baseY, direction);
  }
  g_setAspect(cam, direction == 1 ? ratio : direction == -1 ? 1.0f / ratio : 1.0f);
  ReadTanFov(cam, viewport, tanX, tanY);
}

// ---- Diagnostic: find renderer state that flips on every Render call ------------------------
// Snapshots the GraphicsMTRenderer object before the first eye, after it, and after the second
// eye, and reports 32-bit words that change on each call and change back (double buffering).
constexpr size_t kSnapWords = 4096;  // 16 KB
int g_toggleDiagFrames = 0;          // frames left to sample
std::vector<int> g_toggleHits;

size_t ReadableWords(const void* p) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
  const size_t avail = (static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) - static_cast<const uint8_t*>(p);
  return std::min(kSnapWords, avail / 4);
}

std::vector<int> g_consumedHits;  // changed by the first eye's render, untouched by the second

void ToggleDiag(const void* renderer, const std::vector<uint32_t>& s0, const std::vector<uint32_t>& s1,
                const std::vector<uint32_t>& s2) {
  if (g_toggleHits.size() < s0.size()) g_toggleHits.assign(s0.size(), 0);
  if (g_consumedHits.size() < s0.size()) g_consumedHits.assign(s0.size(), 0);
  for (size_t i = 0; i < s0.size(); ++i) {
    if (s0[i] != s1[i] && s0[i] == s2[i]) ++g_toggleHits[i];
    if (s0[i] != s1[i] && s1[i] == s2[i]) ++g_consumedHits[i];
  }
  if (--g_toggleDiagFrames > 0) return;
  int logged = 0;
  for (size_t i = 0; i < g_toggleHits.size() && logged < 40; ++i)
    if (g_toggleHits[i] >= 5) {
      Log("toggle: renderer+0x%04zX flips every Render (%d/6 frames): %08X -> %08X", i * 4, g_toggleHits[i], s0[i], s1[i]);
      ++logged;
    }
  Log("toggle: %d candidate words (renderer %p)", logged, renderer);
  // "Consumed" words: e.g. a list's end pointer reset to its begin by the first render.
  logged = 0;
  for (size_t i = 0; i + 1 < g_consumedHits.size() && logged < 60; ++i)
    if (g_consumedHits[i] >= 4) {
      const uint64_t before = s0[i] | (uint64_t(i + 1 < s0.size() ? s0[i + 1] : 0) << 32);
      const uint64_t after = s1[i] | (uint64_t(i + 1 < s1.size() ? s1[i + 1] : 0) << 32);
      Log("consumed: renderer+0x%04zX changed by first eye only (%d/6): %016llX -> %016llX", i * 4,
          g_consumedHits[i], (unsigned long long)before, (unsigned long long)after);
      ++logged;
    }
  g_toggleHits.clear();
  g_consumedHits.clear();
}

// ---- Diagnostic: find frame counters --------------------------------------------------------
// Snapshots writable memory (the renderer object and the writable sections of the engine DLLs)
// at the start of consecutive stereo frames and reports 32-bit words that grow by exactly 1 per
// frame. One of them is likely what makes post-processing run only once per frame.
struct Region {
  std::string name;
  const uint32_t* base;
  size_t words;
};
std::vector<Region> g_counterRegions;
std::vector<std::vector<uint32_t>> g_counterSnaps;  // [frame] concatenated words
int g_counterDiagFrames = 0;

void AddModuleDataSections(const wchar_t* module, const char* label) {
  auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(module));
  if (!base) return;
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) {
      char nm[9] = {};
      std::memcpy(nm, sec->Name, 8);
      g_counterRegions.push_back({std::string(label) + "+" + std::to_string(sec->VirtualAddress),
                                  reinterpret_cast<const uint32_t*>(base + sec->VirtualAddress),
                                  sec->Misc.VirtualSize / 4});
    }
}

void CounterDiagFrame(const void* renderer) {
  if (g_counterSnaps.empty()) {
    g_counterRegions.clear();
    g_counterRegions.push_back({"renderer", static_cast<const uint32_t*>(renderer), ReadableWords(renderer)});
    AddModuleDataSections(L"Engine.dll", "Engine.dll");
    AddModuleDataSections(L"Game.dll", "Game.dll");
    AddModuleDataSections(L"Direct3D11.dll", "Direct3D11.dll");
  }
  std::vector<uint32_t> snap;
  for (const Region& r : g_counterRegions) snap.insert(snap.end(), r.base, r.base + r.words);
  g_counterSnaps.push_back(std::move(snap));
  if (--g_counterDiagFrames > 0) return;

  // Words that increase by exactly 1 between every pair of consecutive frames.
  const auto& f = g_counterSnaps;
  int found = 0;
  size_t offset = 0;
  for (const Region& r : g_counterRegions) {
    for (size_t i = 0; i < r.words && found < 60; ++i) {
      bool counter = true;
      for (size_t k = 1; k < f.size() && counter; ++k) counter = f[k][offset + i] == f[k - 1][offset + i] + 1;
      if (counter) {
        Log("counter: %s word +0x%zX (addr %p) = %u", r.name.c_str(), i * 4, r.base + i, f.back()[offset + i]);
        ++found;
      }
    }
    offset += r.words;
  }
  Log("counter: %d candidates over %zu frames", found, f.size());
  g_counterSnaps.clear();
}

// "Consumed" scan over the engine DLLs' writable data: words the first eye's render changes and
// the second leaves alone (per-frame state the first render uses up). Sampled over 6 frames.
int g_globalConsumedFrames = 0;
std::vector<uint32_t> g_gSnap[3];
std::vector<int> g_gHits;

void GlobalSnap(int k) {
  if (g_counterRegions.empty() || g_counterRegions[0].name != "Engine.dll+all") {
    g_counterRegions.clear();
    AddModuleDataSections(L"Engine.dll", "Engine.dll");
    AddModuleDataSections(L"Game.dll", "Game.dll");
    AddModuleDataSections(L"Direct3D11.dll", "Direct3D11.dll");
    g_counterRegions.insert(g_counterRegions.begin(), Region{"Engine.dll+all", nullptr, 0});
  }
  g_gSnap[k].clear();
  for (const Region& r : g_counterRegions)
    if (r.words) g_gSnap[k].insert(g_gSnap[k].end(), r.base, r.base + r.words);
}

void GlobalConsumedFrame() {
  if (g_gHits.size() != g_gSnap[0].size()) g_gHits.assign(g_gSnap[0].size(), 0);
  for (size_t i = 0; i < g_gSnap[0].size(); ++i)
    if (g_gSnap[0][i] != g_gSnap[1][i] && g_gSnap[1][i] == g_gSnap[2][i]) ++g_gHits[i];
  if (--g_globalConsumedFrames > 0) return;
  int found = 0;
  size_t offset = 0;
  for (const Region& r : g_counterRegions) {
    for (size_t i = 0; i < r.words && found < 60; ++i)
      if (g_gHits[offset + i] >= 5) {
        Log("gconsumed: %s word +0x%zX (%p) %08X -> %08X (%d/6)", r.name.c_str(), i * 4, r.base + i,
            g_gSnap[0][offset + i], g_gSnap[1][offset + i], g_gHits[offset + i]);
        ++found;
      }
    offset += r.words;
  }
  Log("gconsumed: %d candidates", found);
  g_gHits.clear();
}

// Per-frame render settings the game sets each frame and the renderer clears after the first
// Render (flags + a float at renderer+0x1E0: target glow / post effects). Saving them before the
// first eye and restoring them before the second gives both eyes glow and FXAA.
constexpr size_t kReplayOffset = 0x1E0, kReplaySize = 16;

// Extra renderer ranges replayed the same way (offset, size), from control.ini replay_extra,
// e.g. "38:8,814:4" (hex). Used to test other state the first render consumes.
struct ReplayRange {
  size_t offset, size;
};
std::vector<ReplayRange> g_replayExtra;
uint8_t g_replayExtraBytes[256];

// Renders the scene once per eye: the game camera is replaced by the eye's camera
// (game camera + head pose), the result is captured for the headset, and the camera is restored.
void RenderStereo(gd::GraphicsMTRenderer* self, const gd::Camera* camera, gd::Region* region,
                  const gd::Viewport* viewport, const gd::Frustum* frustum, unsigned int flags, bool flag2,
                  const gd::WorldFrustum* worldFrustum) {
  const xr::Settings s = xr::GetSettings();
  auto* cam = const_cast<gd::Camera*>(camera);

  float saved[kCoordsCopy];
  std::memcpy(saved, g_getCoords(camera), sizeof(saved));
  const float savedFov = g_getFov(camera);
  const float savedAspect = g_getAspect(camera);
  const float savedFar = g_getFar(camera);
  pointer::GameView gameView;  // the game's own camera: where the pointer becomes a mouse position
  ReadTanFov(camera, viewport, gameView.tanX, gameView.tanY);
  gameView.origin = Read3(saved, kOrigin);
  gameView.right = Read3(saved, kSide) * -1.0f;
  gameView.up = Read3(saved, kUp);
  gameView.forward = Read3(saved, kForward);
  gameView.width = float(g_vpWidth(viewport));
  gameView.height = float(g_vpHeight(viewport));
  if (s.farPlane > 0) g_setFar(cam, s.farPlane);

  // Base frame the head moves in. Levelled by default so looking straight ahead in real life
  // means looking at the game's horizon (look down to see your character).
  Vec3 right = Read3(saved, kSide) * -1.0f, up = Read3(saved, kUp), fwd = Read3(saved, kForward);
  Vec3 origin = Read3(saved, kOrigin);
  {
    // Bring the viewpoint closer to the character along the camera's line of sight (the whole zoom
    // range moves in by the same amount), never closer than a little above the character.
    const float* t = g_cameraTarget(self);
    const Vec3 target = Read3(t, kTargetPos);
    const Vec3 toTarget = target - origin;
    const float dist = std::sqrt(Dot(toTarget, toTarget));
    const bool haveTarget = t[kTargetPos] != 0 || t[kTargetPos + 1] != 0 || t[kTargetPos + 2] != 0;
    if (s.cameraPull > 0 && haveTarget && dist > 1e-3f)
      origin = origin + toTarget * (std::min(s.cameraPull, std::max(0.0f, dist - 4.0f)) / dist);
  }
  if (s.levelHorizon) {
    fwd = Normalize({fwd.x, 0, fwd.z});
    up = {0, 1, 0};
    right = Cross(fwd, up);
  }
  auto toWorld = [&](Vec3 v) { return right * v.x + up * v.y + fwd * v.z; };
  labels::SetStereoFrame(origin, right, up, fwd, s.worldScale);  // for placing labels this frame
  numbers::OnStereoFrame();
  {
    const float* t = g_cameraTarget(self);
    const bool haveTarget = t[kTargetPos] != 0 || t[kTargetPos + 1] != 0 || t[kTargetPos + 2] != 0;
    xr::SetInWorld(haveTarget);
    pointer::Update(origin, right, up, fwd, s.worldScale, Read3(t, kTargetPos), haveTarget, gameView);
  }

  // Frustum objects are built in place by the engine (size unknown, so over-allocate).
  alignas(16) static unsigned char frustumBuf[2][4096];

  if (g_counterDiagFrames > 0) CounterDiagFrame(self);
  const bool globalScan = g_globalConsumedFrames > 0;
  if (globalScan) GlobalSnap(0);
  std::vector<uint32_t> snaps[3];
  const size_t snapWords = g_toggleDiagFrames > 0 ? ReadableWords(self) : 0;
  auto snap = [&](int k) {
    if (snapWords) snaps[k].assign(reinterpret_cast<const uint32_t*>(self), reinterpret_cast<const uint32_t*>(self) + snapWords);
  };
  snap(0);

  uint8_t replayBytes[kReplaySize];
  for (int pass = 0; pass < 2; ++pass) {
    const int eye = s.swapEyeOrder ? 1 - pass : pass;
    const Pose p = xr::EyePose(eye);
    float eyeCoords[kCoordsCopy];
    std::memcpy(eyeCoords, saved, sizeof(eyeCoords));
    Write3(eyeCoords, kSide, toWorld(XrToCamera(Rotate(p.orientation, {1, 0, 0}))) * -1.0f);
    Write3(eyeCoords, kUp, toWorld(XrToCamera(Rotate(p.orientation, {0, 1, 0}))));
    Write3(eyeCoords, kForward, toWorld(XrToCamera(Rotate(p.orientation, {0, 0, -1}))));
    Write3(eyeCoords, kOrigin, origin + toWorld(XrToCamera(p.position)) * s.worldScale);
    g_setCoords(cam, reinterpret_cast<const gd::Coords*>(eyeCoords));
    float tanX, tanY;
    FitFrustum(cam, viewport, eye, s.fovDeg, tanX, tanY);
    float proj[16] = {};
    g_getProj(camera, reinterpret_cast<gd::Mat4*>(proj), viewport);
    static float loggedFov = -1;
    if (eye == 0 && loggedFov != s.fovDeg) {
      loggedFov = s.fovDeg;
      Log("stereo: fov %.0f -> projection diag %.4f %.4f %.4f %.4f, off-centre %.4f %.4f %.4f %.4f", s.fovDeg,
          proj[0], proj[5], proj[10], proj[15], proj[2], proj[6], proj[8], proj[9]);
    }

    const gd::Frustum* eyeFrustum = frustum;
    if (s.recomputeFrustum)
      eyeFrustum = g_getFrustum(camera, reinterpret_cast<gd::Frustum*>(frustumBuf[eye]), viewport);

    if (pass == 0) perf::Mark(perf::kEye0Begin);
    const double t0 = NowMs();
    mask::BeginCapture(unsigned(g_vpWidth(viewport)), unsigned(g_vpHeight(viewport)), s.maskDepthPick);
    mask::RestoreEyeHistory(eye);  // neutralise the engine's previous-depth soft blend (see mask.h)
    perf::SetCurrentEye(eye);
    d3dtrace::SetEye(eye);

    // Per-frame render settings the renderer clears after the first Render (glow / post effects):
    // capture them before the first eye, restore them before the second.
    auto* replay = reinterpret_cast<uint8_t*>(self) + kReplayOffset;
    if (pass == 0) std::memcpy(replayBytes, replay, kReplaySize);
    else std::memcpy(replay, replayBytes, kReplaySize);
    {
      size_t at = 0;
      for (const ReplayRange& r : g_replayExtra) {
        if (at + r.size > sizeof(g_replayExtraBytes)) break;
        auto* p = reinterpret_cast<uint8_t*>(self) + r.offset;
        if (pass == 0) std::memcpy(g_replayExtraBytes + at, p, r.size);
        else std::memcpy(p, g_replayExtraBytes + at, r.size);
        at += r.size;
      }
    }

    g_origRender(self, camera, region, viewport, eyeFrustum, flags, flag2, worldFrustum);
    d3dtrace::SetEye(-1);
    mask::SaveEyeHistory(eye);
    perf::SetCurrentEye(-1);
    mask::EndCapture();
    perf::AddEyeCpu(eye, NowMs() - t0);

    // No camera target (main menu, character select): nothing to centre the fade on.
    const float* target = g_cameraTarget(self);
    const bool haveTarget = target[kTargetPos] != 0 || target[kTargetPos + 1] != 0 || target[kTargetPos + 2] != 0;
    if (s.maskEnabled && haveTarget) {
      mask::Params mp;
      mp.right = toWorld(XrToCamera(Rotate(p.orientation, {1, 0, 0})));
      mp.up = Read3(eyeCoords, kUp);
      mp.forward = Read3(eyeCoords, kForward);
      mp.origin = Read3(eyeCoords, kOrigin);
      mp.tanX = tanX;
      mp.tanY = tanY;
      mp.nearPlane = g_getNear(camera);
      mp.farPlane = g_getFar(camera);
      mp.target = Read3(target, kTargetPos);
      mp.inner = s.maskInner;
      mp.outer = s.maskOuter;
      mp.depthStart = s.maskDepthStart;
      mp.strength = s.maskStrength;
      mp.debug = s.maskDebug;
      if (s.sky) {
        const sky::Layers layers = sky::Get(xr::GameSwapChain());
        mp.skyConstellations = layers.constellations;
        mp.skyBackdrop = layers.backdrop;
        mp.dome = layers.dome;
        mp.domeHorizon = s.skyDomeHorizonDeg * 3.14159265f / 180.0f;
        const float yaw = s.skyBandYawDeg * 3.14159265f / 180.0f;
        // Laid out on the world's axes, not the camera's: turning the camera turns the sky with the ground.
        const Vec3 f0{0, 0, 1}, r0 = Cross(f0, Vec3{0, 1, 0});
        mp.skyForward = f0 * std::cos(yaw) + r0 * std::sin(yaw);
        mp.skyRight = r0 * std::cos(yaw) - f0 * std::sin(yaw);
        mp.bandHalfWidth = s.skyBandWidthDeg * 0.5f * 3.14159265f / 180.0f;
        mp.bandLow = s.skyBandLowDeg * 3.14159265f / 180.0f;
        mp.bandHigh = s.skyBandHighDeg * 3.14159265f / 180.0f;
        for (int k = 0; k < 4; ++k) mp.crop[k] = s.skyCrop[k];
        mp.key = s.skyKey;
        mp.soft = s.skySoft;
        mp.brightness = s.skyBrightness;
        mp.opacity = s.skyOpacity;
        mp.backdropBrightness = s.skyBackdropBrightness;
      }
      xr::ApplyMask(mp);
    }
    {
      // 3D damage numbers, drawn into this eye's image at their world positions.
      numbers::EyeView ev;
      ev.origin = Read3(eyeCoords, kOrigin);
      ev.right = toWorld(XrToCamera(Rotate(p.orientation, {1, 0, 0})));
      ev.up = Read3(eyeCoords, kUp);
      ev.forward = Read3(eyeCoords, kForward);
      ev.tanX = tanX;
      ev.tanY = tanY;
      ev.nearPlane = g_getNear(camera);
      ev.width = float(g_vpWidth(viewport));
      ev.height = float(g_vpHeight(viewport));
      labels::DrawEye(ev);
      numbers::DrawEye(ev);
      pointer::DrawEye(ev);
    }
    xr::CaptureEye(eye, tanX, tanY);
    perf::Mark(pass == 0 ? perf::kEye0End : perf::kEye1End);
    snap(pass + 1);
    if (globalScan) GlobalSnap(pass + 1);
  }
  if (globalScan) GlobalConsumedFrame();
  if (snapWords) ToggleDiag(self, snaps[0], snaps[1], snaps[2]);

  // Both eyes are captured: from here on the game draws its HUD, onto a transparent back buffer.
  xr::PrepareHud();
  d3dtrace::BeginUi();  // from here on the game draws its UI

  g_setFov(cam, savedFov);
  g_setAspect(cam, savedAspect);
  g_setFar(cam, savedFar);
  g_setCoords(cam, reinterpret_cast<const gd::Coords*>(saved));
}

void HookRender(gd::GraphicsMTRenderer* self, const gd::Camera* camera, gd::Region* region,
                const gd::Viewport* viewport, const gd::Frustum* frustum, unsigned int flags,
                bool flag2, const gd::WorldFrustum* worldFrustum) {
  InFlightGuard guard;
  labels::WorldRenderScope worldRender;

  int index;
  {
    std::lock_guard lock(g_mutex);
    index = g_building.count++;
  }

  double t0, t1, t2;
  if (index == 0 && xr::StereoFrameActive()) {
    t0 = NowMs();
    RenderStereo(self, camera, region, viewport, frustum, flags, flag2, worldFrustum);
    t1 = t2 = NowMs();
  } else {
    const Experiments exp = g_exp;
    const bool applyOffset = exp.offsetEnabled && (exp.targetCall < 0 || exp.targetCall == index) &&
                             exp.originIndex >= 0 && exp.originIndex + 2 < kCoordsCopy;

    // Offset experiment: temporarily move the camera, render, then restore it.
    float saved[kCoordsCopy];
    if (applyOffset) {
      float moved[kCoordsCopy];
      std::memcpy(saved, g_getCoords(camera), sizeof(saved));
      std::memcpy(moved, saved, sizeof(moved));
      for (int k = 0; k < 3; ++k) moved[exp.originIndex + k] += exp.offset[k];
      g_setCoords(const_cast<gd::Camera*>(camera), reinterpret_cast<const gd::Coords*>(moved));
    }

    t0 = NowMs();
    g_origRender(self, camera, region, viewport, frustum, flags, flag2, worldFrustum);
    t1 = NowMs();
    if (exp.renderTwice)
      g_origRender(self, camera, region, viewport, frustum, flags, flag2, worldFrustum);
    t2 = NowMs();

    if (applyOffset)
      g_setCoords(const_cast<gd::Camera*>(camera), reinterpret_cast<const gd::Coords*>(saved));
  }

  if (index < kMaxCalls) {
    RenderCall c;
    c.camera = camera;
    c.vpWidth = g_vpWidth(viewport);
    c.vpHeight = g_vpHeight(viewport);
    c.flags = flags;
    c.flag2 = flag2;
    std::memcpy(c.coords, g_getCoords(camera), sizeof(c.coords));
    c.fov = g_getFov(camera);
    c.nearPlane = g_getNear(camera);
    c.farPlane = g_getFar(camera);
    c.aspect = g_getAspect(camera);
    c.ms = t1 - t0;
    c.extraMs = t2 - t1;
    std::lock_guard lock(g_mutex);
    g_building.calls[index] = c;
  }
}

// Experiments can also be driven from %LOCALAPPDATA%\gdvr\control.ini (section [exp]), which is
// re-read whenever it changes. Handy because the game reads the mouse directly, so the UI
// does not always get clicks.
void PollControlFile() {
  static const std::wstring path = [] {
    wchar_t base[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    return std::wstring(base) + L"\\gdvr\\control.ini";
  }();
  static FILETIME lastWrite = {};

  WIN32_FILE_ATTRIBUTE_DATA attr;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) ||
      CompareFileTime(&attr.ftLastWriteTime, &lastWrite) == 0)
    return;
  lastWrite = attr.ftLastWriteTime;

  const wchar_t* section = L"exp";
  auto getFloat = [&](const wchar_t* key, float def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, path.c_str());
    return buf[0] ? float(_wtof(buf)) : def;
  };
  auto getInt = [&](const wchar_t* key, int def) {
    return int(GetPrivateProfileIntW(section, key, def, path.c_str()));
  };
  g_exp.offsetEnabled = getInt(L"offset_enabled", g_exp.offsetEnabled) != 0;
  g_exp.originIndex = getInt(L"origin_index", g_exp.originIndex);
  g_exp.offset[0] = getFloat(L"offset_x", g_exp.offset[0]);
  g_exp.offset[1] = getFloat(L"offset_y", g_exp.offset[1]);
  g_exp.offset[2] = getFloat(L"offset_z", g_exp.offset[2]);
  g_exp.targetCall = getInt(L"target_call", g_exp.targetCall);
  g_exp.renderTwice = getInt(L"render_twice", g_exp.renderTwice) != 0;

  // [vr] section: headset output settings
  section = L"vr";
  xr::Settings& vr = xr::GetSettings();
  vr.mode = xr::Mode(std::clamp(getInt(L"mode", int(vr.mode)), 0, 2));
  vr.worldScale = getFloat(L"world_scale", vr.worldScale);
  vr.fovDeg = getFloat(L"fov_deg", vr.fovDeg);  // 0 = fit the headset automatically
  if (vr.fovDeg > 0) vr.fovDeg = std::clamp(vr.fovDeg, 10.0f, 170.0f);
  vr.levelHorizon = getInt(L"level_horizon", vr.levelHorizon) != 0;
  vr.recomputeFrustum = getInt(L"recompute_frustum", vr.recomputeFrustum) != 0;
  vr.screenDistance = getFloat(L"screen_distance", vr.screenDistance);
  vr.screenWidth = getFloat(L"screen_width", vr.screenWidth);
  vr.bench = getInt(L"bench", vr.bench) != 0;
  vr.keepRunningUnfocused = getInt(L"keep_running_unfocused", vr.keepRunningUnfocused) != 0;
  mask::SetDepthHistoryMode(std::clamp(getInt(L"depth_history", mask::DepthHistoryMode()), 0, 2));
  vr.swapEyeOrder = getInt(L"swap_eye_order", vr.swapEyeOrder) != 0;
  {
    wchar_t buf[256];
    GetPrivateProfileStringW(L"vr", L"replay_extra", L"", buf, 256, path.c_str());
    g_replayExtra.clear();
    for (wchar_t* tok = buf; tok && *tok;) {
      wchar_t* end = nullptr;
      const size_t off = size_t(wcstoull(tok, &end, 16));
      size_t size = 4;
      if (end && *end == L':') size = size_t(wcstoull(end + 1, &end, 16));
      if (off < 0x4000 && size && size <= 64) g_replayExtra.push_back({off, size});
      tok = end && *end == L',' ? end + 1 : nullptr;
    }
    if (!g_replayExtra.empty()) Log("replay_extra: %zu ranges", g_replayExtra.size());
  }
  static int lastGlobal = 0;
  if (int t = getInt(L"diag_gconsumed", lastGlobal); t != lastGlobal) {
    lastGlobal = t;
    g_globalConsumedFrames = 6;
  }
  static int lastSkip = -2;
  if (int sk = getInt(L"skip_segment", -1); sk != lastSkip) {
    lastSkip = sk;
    d3dtrace::SetSkipSegment(sk);
  }
  static int lastDump = 0;
  if (int t = getInt(L"dump_eyes", lastDump); t != lastDump) {
    lastDump = t;
    xr::RequestEyeDump();
  }
  static int lastTrace = 0;
  if (int t = getInt(L"diag_trace", lastTrace); t != lastTrace) {
    lastTrace = t;
    d3dtrace::Arm();
  }
  static int lastCounterDiag = 0;
  if (int t = getInt(L"diag_counter", lastCounterDiag); t != lastCounterDiag) {
    lastCounterDiag = t;
    g_counterDiagFrames = 8;
  }
  static int lastToggleDiag = 0;
  if (int t = getInt(L"diag_toggle", lastToggleDiag); t != lastToggleDiag) {
    lastToggleDiag = t;
    g_toggleDiagFrames = 6;
  }
  vr.hud = getInt(L"hud", vr.hud) != 0;
  vr.hudDistance = getFloat(L"hud_distance", vr.hudDistance);
  vr.hudWidth = getFloat(L"hud_width", vr.hudWidth);
  vr.hudDown = getFloat(L"hud_down", vr.hudDown);
  vr.hudTiltDeg = getFloat(L"hud_tilt", vr.hudTiltDeg);
  vr.hudSideShift = getFloat(L"hud_side_shift", vr.hudSideShift);
  vr.hudSideWidth = getFloat(L"hud_side_width", vr.hudSideWidth);
  vr.hudSideHeight = getFloat(L"hud_side_height", vr.hudSideHeight);
  vr.hudMenuCoverage = getFloat(L"hud_menu_coverage", vr.hudMenuCoverage);
  vr.labelsInWorld = getInt(L"labels_in_world", vr.labelsInWorld) != 0;
  vr.damageNumbers3d = getInt(L"damage_numbers_3d", vr.damageNumbers3d) != 0;
  vr.cameraPull = getFloat(L"camera_pull", vr.cameraPull);
  vr.sky = getInt(L"sky", vr.sky) != 0;
  vr.skySource = getInt(L"sky_source", vr.skySource);
  vr.skyDomeHorizonDeg = getFloat(L"sky_dome_horizon", vr.skyDomeHorizonDeg);
  vr.skyIncomplete = getFloat(L"sky_incomplete", vr.skyIncomplete);
  vr.skyGlow = getFloat(L"sky_glow", vr.skyGlow);
  vr.credit = getInt(L"credit", vr.credit) != 0;
  {
    wchar_t wide[256];
    GetPrivateProfileStringW(section, L"sky_skip_textures", L"", wide, 256, path.c_str());
    if (wide[0]) WideCharToMultiByte(CP_UTF8, 0, wide, -1, vr.skySkip, sizeof(vr.skySkip), nullptr, nullptr);
  }
  vr.skyBandWidthDeg = getFloat(L"sky_band_width", vr.skyBandWidthDeg);
  vr.skyBandLowDeg = getFloat(L"sky_band_low", vr.skyBandLowDeg);
  vr.skyBandHighDeg = getFloat(L"sky_band_high", vr.skyBandHighDeg);
  vr.skyBandYawDeg = getFloat(L"sky_band_yaw", vr.skyBandYawDeg);
  vr.skyCrop[0] = getFloat(L"sky_crop_left", vr.skyCrop[0]);
  vr.skyCrop[1] = getFloat(L"sky_crop_top", vr.skyCrop[1]);
  vr.skyCrop[2] = getFloat(L"sky_crop_right", vr.skyCrop[2]);
  vr.skyCrop[3] = getFloat(L"sky_crop_bottom", vr.skyCrop[3]);
  vr.skyKey = getFloat(L"sky_key", vr.skyKey);
  vr.skySoft = getFloat(L"sky_soft", vr.skySoft);
  vr.skyBrightness = getFloat(L"sky_brightness", vr.skyBrightness);
  vr.skyOpacity = getFloat(L"sky_opacity", vr.skyOpacity);
  vr.skyBackdropBrightness = getFloat(L"sky_backdrop_brightness", vr.skyBackdropBrightness);
  vr.itemLabels3d = getInt(L"item_labels_3d", vr.itemLabels3d) != 0;
  vr.pointer = getInt(L"pointer", vr.pointer) != 0;
  vr.pointerHand = getInt(L"pointer_hand", vr.pointerHand) != 0 ? 1 : 0;
  vr.pureController = getInt(L"pure_controller", vr.pureController) != 0;
  vr.hudSideOutline = getInt(L"hud_side_outline", vr.hudSideOutline) != 0;
  vr.hudBrightness = getFloat(L"hud_brightness", vr.hudBrightness);
  vr.hudOpacity = getFloat(L"hud_opacity", vr.hudOpacity);
  vr.hudDayAdapt = getInt(L"hud_day_adapt", vr.hudDayAdapt) != 0;
  vr.hudHalo = getFloat(L"hud_halo", vr.hudHalo);
  vr.hudHaloRadius = getFloat(L"hud_halo_radius", vr.hudHaloRadius);
  vr.hudHaloDay = getFloat(L"hud_halo_day", vr.hudHaloDay);
  vr.hudHaloRadiusDay = getFloat(L"hud_halo_radius_day", vr.hudHaloRadiusDay);
  vr.hudDayFrom = getFloat(L"hud_day_from", vr.hudDayFrom);
  vr.hudDayTo = getFloat(L"hud_day_to", vr.hudDayTo);
  vr.hudAlphaBoost = getFloat(L"hud_alpha_boost", vr.hudAlphaBoost);
  vr.hudForceDaylight = getInt(L"hud_force_daylight", vr.hudForceDaylight) != 0;
  vr.farPlane = getFloat(L"far_plane", vr.farPlane);
  vr.maskEnabled = getInt(L"mask", vr.maskEnabled) != 0;
  vr.maskInner = getFloat(L"mask_inner", vr.maskInner);
  vr.maskOuter = getFloat(L"mask_outer", vr.maskOuter);
  vr.maskDepthStart = getFloat(L"mask_depth_start", vr.maskDepthStart);
  vr.maskStrength = getFloat(L"mask_strength", vr.maskStrength);
  vr.maskDebug = getInt(L"mask_debug", vr.maskDebug) != 0;
  vr.maskDepthPick = getInt(L"mask_depth_pick", vr.maskDepthPick);
  static int lastRecenter = 0;
  if (int r = getInt(L"recenter", lastRecenter); r != lastRecenter) {
    lastRecenter = r;
    xr::RequestRecenter();
  }
  Log("control.ini applied (vr mode %d, scale %.1f, fov %.0f)", int(vr.mode), vr.worldScale, vr.fovDeg);
}

template <typename T>
bool Resolve(HMODULE mod, const char* name, T& out) {
  out = reinterpret_cast<T>(GetProcAddress(mod, name));
  if (!out) Log("MISSING export: %s", name);
  return out != nullptr;
}

}  // namespace

bool Install() {
  HMODULE engine = GetModuleHandleW(L"Engine.dll");
  if (!engine) {
    Log("Engine.dll not loaded");
    return false;
  }

  void* render = nullptr;
  bool ok = Resolve(engine, gd::kRendererRender, render);
  ok &= Resolve(engine, gd::kCameraGetCoords, g_getCoords);
  ok &= Resolve(engine, gd::kCameraSetCoords, g_setCoords);
  ok &= Resolve(engine, gd::kCameraGetFOV, g_getFov);
  ok &= Resolve(engine, gd::kCameraGetNear, g_getNear);
  ok &= Resolve(engine, gd::kCameraGetFar, g_getFar);
  ok &= Resolve(engine, gd::kCameraGetAspect, g_getAspect);
  ok &= Resolve(engine, gd::kViewportGetWidth, g_vpWidth);
  ok &= Resolve(engine, gd::kViewportGetHeight, g_vpHeight);
  ok &= Resolve(engine, gd::kCameraSetFOV, g_setFov);
  ok &= Resolve(engine, gd::kCameraSetAspect, g_setAspect);
  ok &= Resolve(engine, gd::kCameraSetFar, g_setFar);
  ok &= Resolve(engine, gd::kRendererGetCameraTarget, g_cameraTarget);
  ok &= Resolve(engine, gd::kCameraGetProjection, g_getProj);
  ok &= Resolve(engine, gd::kCameraGetFrustum, g_getFrustum);
  if (!ok) return false;


  if (void* isActive = reinterpret_cast<void*>(GetProcAddress(engine, gd::kWinWindowIsActive)))
    MH_CreateHook(isActive, reinterpret_cast<void*>(&HookIsActive), reinterpret_cast<void**>(&g_origIsActive));
  else
    Log("MISSING export: WinWindow::IsActive (game will pause when unfocused)");

  MH_STATUS st = MH_CreateHook(render, reinterpret_cast<void*>(&HookRender),
                               reinterpret_cast<void**>(&g_origRender));
  if (st != MH_OK) {
    Log("MH_CreateHook(GraphicsMTRenderer::Render) failed: %s", MH_StatusToString(st));
    return false;
  }
  Log("Engine hooks created (GraphicsMTRenderer::Render @ %p)", render);
  perf::InstallStageHooks(engine);
  labels::Install(engine);
  numbers::Install(engine);
  return true;
}

void OnFramePresented() {
  labels::OnFramePresented();
  Frame frame;
  {
    std::lock_guard lock(g_mutex);
    g_published = g_building;
    g_building.count = 0;
    frame = g_published;
  }

  // Once a second, write the main camera to the log so results can be read without the UI.
  static double lastLog = 0;
  const double now = NowMs();
  if (now - lastLog < 1000.0 || frame.count == 0) return;
  lastLog = now;
  PollControlFile();
  const RenderCall& c = frame.calls[0];
  const float* f = c.coords;
  Log("calls=%d vp=%dx%d fov=%.4f near=%.2f far=%.2f render=%.2fms dup=%.2fms | offset=%d idx=%d call=%d (%.2f %.2f %.2f) twice=%d",
      frame.count, c.vpWidth, c.vpHeight, c.fov, c.nearPlane, c.farPlane, c.ms, c.extraMs, int(g_exp.offsetEnabled),
      g_exp.originIndex, g_exp.targetCall, g_exp.offset[0], g_exp.offset[1], g_exp.offset[2], int(g_exp.renderTwice));
  Log("  coords %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", f[0], f[1],
      f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
}

int InFlight() { return g_inFlight.load(); }

void DrawDebugUI() {
  const float scale = ImGui::GetIO().FontGlobalScale;
  ImGui::SetNextWindowSize(ImVec2(520 * scale, 640 * scale), ImGuiCond_FirstUseEver);
  ImGui::Begin("Grim Dawn VR " GDVR_VERSION "  (F8 hide)###gdvr");
  xr::DrawDebugUI();
  if (ImGui::CollapsingHeader("Diagnostics")) {
    xr::DrawDiagnostics();
    labels::DrawDebugUI();
    numbers::DrawDebugUI();
    pointer::DrawDebugUI();
    input::DrawDebugUI();
  }
  ImGui::End();
}

}  // namespace gdvr::engine
