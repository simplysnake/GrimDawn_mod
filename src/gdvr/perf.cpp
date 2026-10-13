#include "perf.h"

#include <d3d11.h>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "log.h"

namespace gdvr::perf {

namespace {

// ---- CPU stage timing ------------------------------------------------------------------
// One generic detour serves every stage. It forwards 8 integer/pointer arguments, so it is
// only used for functions with at most 8 parameters (including `this`) and no float
// parameters (those travel in XMM registers, which the timing calls may clobber).
using GenericFn = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                uintptr_t);

struct Stage {
  const char* name;
  const char* symbol;
  bool boolArg = false;                   // log how many calls passed true
  std::atomic<int> trueCalls{0};
  GenericFn original = nullptr;
  std::atomic<int64_t> ticks{0};
  std::atomic<int> calls{0};
  std::atomic<int> eyeCalls[2] = {0, 0};  // calls made while rendering eye 0 / eye 1
};

Stage g_stages[] = {
    {"RenderCamera", "?RenderCamera@GraphicsMTRenderer@GAME@@IEAAXAEBVCamera@2@PEAVRegion@2@AEBVViewport@2@AEBVFrustum@2@W4RenderFlag@2@_NPEBVWorldFrustum@2@@Z"},
    {"BuildScene", "?BuildScene@GraphicsMTRenderer@GAME@@IEAAX_NPEBVWorldFrustum@2@AEAVGraphicsScene@2@AEAV?$vector@PEAVRegion@GAME@@@mem@@@Z"},
    {"BuildShadows", "?BuildShadows@GraphicsMTRenderer@GAME@@IEAAXAEBVGraphicsScene@2@AEAV?$vector@PEAVGraphicsMTJob_BuildShadow@GAME@@@mem@@_N@Z"},
    {"BuildReflections", "?BuildReflections@GraphicsMTRenderer@GAME@@MEAAXAEBVGraphicsScene@2@AEAV?$vector@PEAVGraphicsMTJob_BuildReflection@GAME@@@mem@@_N@Z"},
    {"WaitForJobs", "?WaitForJobs@GraphicsMTRenderer@GAME@@IEAAXXZ"},
    {"ProcessJobs", "?ProcessJobs@GraphicsMTRenderer@GAME@@IEAAXXZ"},
    {"SetupLights", "?SetupLights@GraphicsMTRenderer@GAME@@IEAAPEBULightEntry@2@AEAVGraphicsScene@2@@Z"},
    {"RenderDepth", "?RenderDepth@GraphicsMTRenderer@GAME@@IEAAXAEAVGraphicsScene@2@AEAV?$vector@URenderablePass@GAME@@@mem@@@Z"},
    {"RenderShadow", "?RenderShadow@GraphicsMTRenderer@GAME@@IEAAXPEBULightEntry@2@AEAVGraphicsScene@2@@Z"},
    {"RenderScene", "?RenderScene@GraphicsMTRenderer@GAME@@IEAAXAEAVGraphicsScene@2@@Z"},
    {"RenderReflection", "?RenderReflection@GraphicsMTRenderer@GAME@@IEAAXAEAVGraphicsScene@2@PEBULightEntry@2@PEAUReflectionGroup@2@@Z"},
    {"RenderTransparent", "?RenderTransparentFeatures@GraphicsMTRenderer@GAME@@IEAAXAEAVGraphicsScene@2@AEAV?$vector@URenderablePass@GAME@@@mem@@@Z"},
    {"RenderPostObjects", "?RenderPostObjects@GraphicsMTRenderer@GAME@@IEAAXAEAVGraphicsScene@2@@Z"},
    {"RenderFinalScene", "?RenderFinalScene@GraphicsMTRenderer@GAME@@MEAAXAEAVGraphicsScene@2@@Z"},
    {"RenderPostEffects", "?RenderPostEffects@GraphicsMTRenderer@GAME@@IEAAXAEBVCamera@2@PEBVRegion@2@AEBVViewport@2@@Z"},
    {"FrameBufferCopy", "?UpdateFrameBufferCopy@GraphicsMTRenderer@GAME@@IEAAXXZ"},
    {"RenderBackground", "?RenderBackground@GraphicsMTRenderer@GAME@@IEBAXAEBVViewport@2@@Z"},
    {"Def.PrepareScene", "?ExecutePrepareScene@GraphicsDeferredRenderer@GAME@@UEAAXAEAVGraphicsScene@2@PEBULightEntry@2@@Z"},
    {"Def.SceneGlow", "?RenderSceneGlow@GraphicsDeferredRenderer@GAME@@AEAAXAEBVGraphicsScene@2@@Z"},
    {"Def.Fog", "?RenderFog@GraphicsDeferredRenderer@GAME@@AEAAXAEBVGraphicsScene@2@@Z"},
    {"Def.FinalScene", "?RenderFinalScene@GraphicsDeferredRenderer@GAME@@UEAAXAEAVGraphicsScene@2@@Z"},
    {"Def.Stencil", "?RenderStencilBuffer@GraphicsDeferredRenderer@GAME@@AEAAXAEBVViewport@2@HAEBVColor@2@@Z"},
    // Highlight / outline setters (diagnostic for "glow only in one eye"); first arg is a bool.
    {"Mesh.SetOutlined", "?SetOutlined@GraphicsMeshInstance@GAME@@QEAAX_N@Z", true},
    {"Mesh.SetOutlineColor", "?SetOutlineColor@GraphicsMeshInstance@GAME@@QEAAXAEBVColor@2@@Z"},
    {"Actor.EnableOutline", "?EnableOutline@Actor@GAME@@UEAAX_N@Z", true},
    {"Actor.SetHighlight", "?SetHighlight@Actor@GAME@@UEAAX_N@Z", true},
};
constexpr int kStageCount = int(std::size(g_stages));

std::atomic<int> g_inFlight{0};
std::atomic<int> g_currentEye{-1};

int64_t Ticks() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
double TicksToMs(int64_t t) {
  static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
  return 1000.0 * double(t) / freq;
}

template <int I>
uintptr_t StageDetour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g,
                      uintptr_t h) {
  ++g_inFlight;
  const int64_t t0 = Ticks();
  const uintptr_t r = g_stages[I].original(a, b, c, d, e, f, g, h);
  g_stages[I].ticks += Ticks() - t0;
  ++g_stages[I].calls;
  if (g_stages[I].boolArg && (b & 0xFF)) ++g_stages[I].trueCalls;
  if (const int eye = g_currentEye.load(); eye >= 0) ++g_stages[I].eyeCalls[eye];
  --g_inFlight;
  return r;
}

template <size_t... I>
constexpr auto MakeDetours(std::index_sequence<I...>) {
  return std::array<GenericFn, sizeof...(I)>{&StageDetour<int(I)>...};
}
const auto g_detours = MakeDetours(std::make_index_sequence<kStageCount>{});

// ---- GPU timing ------------------------------------------------------------------------
constexpr int kSlots = 6;  // frames in flight before results are read back

struct GpuSlot {
  ID3D11Query* disjoint = nullptr;
  ID3D11Query* stamps[kGpuMarkCount] = {};
  bool marked[kGpuMarkCount] = {};
  bool pending = false;
};

ID3D11DeviceContext* g_context = nullptr;
GpuSlot g_slots[kSlots];
int g_slot = 0;
bool g_gpuReady = false;
bool g_slotOpen = false;

// ---- Per-second aggregation ------------------------------------------------------------
struct Window {
  int frames = 0;
  double frameMs = 0, waitMs = 0;
  double eyeCpu[2] = {};
  int gpuSamples = 0;
  double gpuEye[2] = {}, gpuPost = 0, gpuFrame = 0;
};
Window g_win;
int64_t g_lastPresent = 0;
int64_t g_windowStart = 0;
uint64_t g_lastGpuPresent = 0;
int64_t g_stageBase[kStageCount] = {};

bool InitGpu(ID3D11Device* device) {
  D3D11_QUERY_DESC dq{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
  D3D11_QUERY_DESC tq{D3D11_QUERY_TIMESTAMP, 0};
  for (GpuSlot& s : g_slots) {
    if (FAILED(device->CreateQuery(&dq, &s.disjoint))) return false;
    for (ID3D11Query*& q : s.stamps)
      if (FAILED(device->CreateQuery(&tq, &q))) return false;
  }
  return true;
}

void CollectGpu(GpuSlot& s) {
  if (!s.pending) return;
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
  if (g_context->GetData(s.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
  s.pending = false;
  uint64_t t[kGpuMarkCount] = {};
  for (int m = 0; m < kGpuMarkCount; ++m)
    if (s.marked[m] && g_context->GetData(s.stamps[m], &t[m], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
      return;
  if (dj.Disjoint || !dj.Frequency || !s.marked[kPresent]) return;
  auto ms = [&](uint64_t a, uint64_t b) { return 1000.0 * double(int64_t(b - a)) / double(dj.Frequency); };
  if (g_lastGpuPresent) g_win.gpuFrame += ms(g_lastGpuPresent, t[kPresent]);
  g_lastGpuPresent = t[kPresent];
  if (s.marked[kEye0Begin] && s.marked[kEye0End] && s.marked[kEye1End]) {
    g_win.gpuEye[0] += ms(t[kEye0Begin], t[kEye0End]);
    g_win.gpuEye[1] += ms(t[kEye0End], t[kEye1End]);
    g_win.gpuPost += ms(t[kEye1End], t[kPresent]);
    ++g_win.gpuSamples;
  }
}

void LogWindow(double seconds) {
  const Window& w = g_win;
  if (!w.frames) return;
  const double n = w.frames;
  char line[4096];
  int len = snprintf(line, sizeof(line), "perf: %.0f fps | cpu frame %.1f ms, xr wait %.1f, eye cpu %.1f + %.1f",
                     n / seconds, w.frameMs / n, w.waitMs / n, w.eyeCpu[0] / n, w.eyeCpu[1] / n);
  if (w.gpuSamples) {
    const double g = w.gpuSamples;
    len += snprintf(line + len, sizeof(line) - len, " | gpu eye %.1f + %.1f, after eyes %.1f, gpu frame %.1f",
                    w.gpuEye[0] / g, w.gpuEye[1] / g, w.gpuPost / g, w.gpuFrame / g);
  }
  Log("%s", line);

  // Stages: average ms per frame and calls per frame, biggest first is easy to read by eye.
  len = snprintf(line, sizeof(line), "perf stages (ms/frame, calls):");
  for (int i = 0; i < kStageCount; ++i) {
    const int64_t ticks = g_stages[i].ticks.load();
    const int calls = g_stages[i].calls.exchange(0);
    const double msPerFrame = TicksToMs(ticks - g_stageBase[i]) / n;
    g_stageBase[i] = ticks;
    const int e0 = g_stages[i].eyeCalls[0].exchange(0), e1 = g_stages[i].eyeCalls[1].exchange(0);
    const int t = g_stages[i].trueCalls.exchange(0);
    if (calls)
      len += snprintf(line + len, sizeof(line) - len, " %s %.2f (%.1f; L%.1f R%.1f%s)", g_stages[i].name, msPerFrame,
                      calls / n, e0 / n, e1 / n, g_stages[i].boolArg ? (" true " + std::to_string(t)).c_str() : "");
  }
  Log("%s", line);
}

}  // namespace

bool InstallStageHooks(HMODULE engine) {
  int hooked = 0;
  for (int i = 0; i < kStageCount; ++i) {
    void* target = reinterpret_cast<void*>(GetProcAddress(engine, g_stages[i].symbol));
    if (!target) {
      Log("perf: missing export %s", g_stages[i].name);
      continue;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(g_detours[i]),
                      reinterpret_cast<void**>(&g_stages[i].original)) == MH_OK)
      ++hooked;
  }
  Log("perf: %d/%d render stages hooked", hooked, kStageCount);
  return hooked > 0;
}

void Mark(GpuMark mark) {
  if (!g_gpuReady || !g_slotOpen) return;
  GpuSlot& s = g_slots[g_slot];
  g_context->End(s.stamps[mark]);
  s.marked[mark] = true;
}

void AddEyeCpu(int eye, double ms) { g_win.eyeCpu[eye & 1] += ms; }

void SetCurrentEye(int eye) { g_currentEye = eye; }

void OnPresent(ID3D11Device* device, ID3D11DeviceContext* context, double xrWaitMs) {
  if (!g_context) {
    g_context = context;
    g_gpuReady = InitGpu(device);
    if (!g_gpuReady) Log("perf: GPU timer queries unavailable");
  }

  // Close this frame's GPU slot and open the next one.
  if (g_gpuReady) {
    if (g_slotOpen) {
      Mark(kPresent);
      g_context->End(g_slots[g_slot].disjoint);
      g_slots[g_slot].pending = true;
      g_slot = (g_slot + 1) % kSlots;
    }
    GpuSlot& next = g_slots[g_slot];
    CollectGpu(next);   // oldest slot: results should be ready by now
    next.pending = false;
    for (bool& m : next.marked) m = false;
    g_context->Begin(next.disjoint);
    g_slotOpen = true;
    for (int i = 1; i < kSlots; ++i) CollectGpu(g_slots[(g_slot + i) % kSlots]);
  }

  const int64_t now = Ticks();
  if (g_lastPresent) {
    g_win.frameMs += TicksToMs(now - g_lastPresent);
    g_win.waitMs += xrWaitMs;
    ++g_win.frames;
  }
  g_lastPresent = now;
  if (!g_windowStart) g_windowStart = now;
  const double seconds = TicksToMs(now - g_windowStart) / 1000.0;
  if (seconds >= 1.0) {
    LogWindow(seconds);
    g_win = Window{};
    g_windowStart = now;
  }
}

int InFlight() { return g_inFlight.load(); }

void Shutdown() {
  for (GpuSlot& s : g_slots) {
    if (s.disjoint) s.disjoint->Release();
    for (ID3D11Query*& q : s.stamps)
      if (q) q->Release();
    s = GpuSlot{};
  }
  g_context = nullptr;
  g_gpuReady = false;
  g_slotOpen = false;
}

}  // namespace gdvr::perf
