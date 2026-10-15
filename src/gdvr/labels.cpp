#include "labels.h"

#include <MinHook.h>
#include <imgui.h>
#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "d3dtrace.h"
#include "log.h"
#include "numbers.h"
#include "overlay.h"
#include "xr.h"

namespace gdvr::labels {

namespace {

// Vec2 WorldCamera::Project(const WorldVec3&, const Viewport&) const and Camera::Project(const Vec3&, ...).
// Both return a class by value (hidden result pointer), so all arguments are pointers: a 4-pointer
// pass-through detour is exact for either convention.
constexpr char kWorldCameraProject[] =
    "?Project@WorldCamera@GAME@@QEBA?AVVec2@2@AEBVWorldVec3@2@AEBVViewport@2@@Z";
constexpr char kCameraProject[] = "?Project@Camera@GAME@@QEBA?AVVec2@2@AEBVVec3@2@AEBVViewport@2@@Z";
using ProjectFn = void* (*)(void*, void*, void*, void*);

ProjectFn g_origWorldProject = nullptr;
ProjectFn g_origCameraProject = nullptr;
std::atomic<int> g_inFlight{0};
thread_local int t_worldRenderDepth = 0;
std::atomic<DWORD> g_renderThread{0};
unsigned long long g_frames = 0;  // presented frames

// The latest stereo view. The UI lays out its labels before the world is rendered, so it uses the
// previous frame's view; it counts as current while stereo frames keep coming.
struct StereoFrame {
  unsigned long long frame = 0;  // g_frames when it was set
  Vec3 origin, right, up, fwd;
  float scale = 1;
};
StereoFrame g_frame;  // render thread only

using ViewportGetIntFn = int (*)(const void* viewport);
ViewportGetIntFn g_vpWidth = nullptr;
ViewportGetIntFn g_vpHeight = nullptr;
HMODULE g_exe = nullptr;
std::atomic<unsigned long long> g_placed{0}, g_hidden{0};

// Labels (health bars, names, damage numbers, item names) are projected by the game's UI code in
// Grim Dawn.exe after the world is drawn, then drawn in one batch with the rest of the UI. The UI
// is shown on the HUD panel, so instead of the flat-screen position, give each label the point
// where your line of sight to its world position crosses the panel: it then appears over its
// monster or item. Returns false to keep the game's own result.
bool PlaceOnPanel(const float* world, const void* viewport, float out[2]) {
  if (g_frames > g_frame.frame + 2 || g_frame.frame == 0 || !xr::GetSettings().labelsInWorld || !g_vpWidth || !g_vpHeight) return false;
  const Vec3 v = (Vec3{world[0], world[1], world[2]} - g_frame.origin) * (1.0f / g_frame.scale);
  const Vec3 rel{Dot(v, g_frame.right), Dot(v, g_frame.up), -Dot(v, g_frame.fwd)};  // camera -> headset axes
  float u, w;
  if (!xr::HudPanelPoint(rel, u, w)) {
    out[0] = out[1] = -100000.0f;  // behind you: off the panel
    ++g_hidden;
    return true;
  }
  out[0] = u * float(g_vpWidth(viewport));
  out[1] = w * float(g_vpHeight(viewport));
  ++g_placed;
  return true;
}

// ---- Item labels (names of items on the ground, chests, ...) -------------------------------------
// Grim Dawn.exe+0x28C070 lays out one world label: bool (label widget, entity, const std::wstring& text,
// Rect* box {x, y, w, h}, Rect* placed) - `placed` (5th argument, on the stack) receives the box after
// it is moved clear of other labels. It projects the label's anchor above the entity (WorldCamera::Project,
// returning to +0x28C1AC) and measures the text in the widget's font style ([widget+0x10]+0x88,
// a style name such as the item's rarity). In stereo the mod records the label and moves the
// game's flat box off-screen, then draws the label itself into each eye at the anchor.
constexpr uintptr_t kLabelLayoutRva = 0x28C070;
constexpr uintptr_t kLabelProjectReturnRva = 0x28C1AC;
constexpr unsigned char kLabelLayoutPrologue[] = {0x48, 0x8B, 0xC4, 0x55, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xC8};
using LabelLayoutFn = void* (*)(void* widget, void* entity, const void* text, float* rect, float* placed);
LabelLayoutFn g_origLabelLayout = nullptr;

struct ItemLabel {
  const void* entity;
  char text[96];  // UTF-8, colour tags removed
  char style[48];
  unsigned color;  // from the text's colour tag ({^y} etc.)
  float pos[3];
  bool havePos;
  double seenMs;
};
std::mutex g_itemMutex;
std::vector<ItemLabel> g_items;
thread_local ItemLabel* t_currentLabel = nullptr;
std::atomic<unsigned long long> g_itemLayouts{0}, g_itemHidden{0}, g_itemReadable{0}, g_itemAnchored{0};
std::atomic<int> g_itemDetailLogs{0};
std::vector<std::string> g_stylesSeen;  // under g_itemMutex

double ItemNowMs() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

// MSVC std::basic_string: inline buffer or pointer (16 bytes), size, capacity.
template <typename Char>
bool ReadStdString(const unsigned char* str, Char* out, size_t outMax) {
  __try {
    const size_t size = *reinterpret_cast<const size_t*>(str + 0x10);
    const size_t cap = *reinterpret_cast<const size_t*>(str + 0x18);
    constexpr size_t inlineCap = 16 / sizeof(Char) - 1;
    if (size > 512 || cap < size) return false;
    const Char* chars = cap > inlineCap ? *reinterpret_cast<Char* const*>(str) : reinterpret_cast<const Char*>(str);
    const size_t n = std::min(size, outMax - 1);
    std::memcpy(out, chars, n * sizeof(Char));
    out[n] = 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool ReadPointer(const void* at, const void*& out) {
  __try {
    out = *static_cast<const void* const*>(at);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool ItemLabels3dOn();  // below

// Grim Dawn's text colour tags ({^y}Name): the letter picks the colour (item rarity for loot).
unsigned TagColor(wchar_t c) {
  switch (c | 0x20) {
    case 'y': return IM_COL32(255, 240, 70, 0);    // magic
    case 'g': return IM_COL32(64, 230, 64, 0);     // rare
    case 'b': return IM_COL32(64, 160, 255, 0);    // epic
    case 'p': return IM_COL32(197, 92, 255, 0);    // legendary
    case 'o': return IM_COL32(255, 170, 60, 0);
    case 'r': return IM_COL32(255, 80, 70, 0);
    case 'a': return IM_COL32(80, 230, 230, 0);
    case 'k': return IM_COL32(150, 150, 150, 0);
    default: return IM_COL32(235, 235, 235, 0);
  }
}

// UTF-16 label text -> UTF-8 without the {^x} tags; the first tag sets the colour.
void ConvertLabelText(const wchar_t* in, char* out, size_t outMax, unsigned& color) {
  color = IM_COL32(235, 235, 235, 0);
  bool colored = false;
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 4 < outMax; ++i) {
    if (in[i] == L'{' && in[i + 1] == L'^' && in[i + 2] && in[i + 3] == L'}') {
      if (!colored) color = TagColor(in[i + 2]);
      colored = true;
      i += 3;
      continue;
    }
    unsigned cp = in[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
      ++i;
    }
    if (cp < 0x80) {
      out[o++] = char(cp);
    } else if (cp < 0x800) {
      out[o++] = char(0xC0 | (cp >> 6));
      out[o++] = char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out[o++] = char(0xE0 | (cp >> 12));
      out[o++] = char(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = char(0x80 | (cp & 0x3F));
    } else {
      out[o++] = char(0xF0 | (cp >> 18));
      out[o++] = char(0x80 | ((cp >> 12) & 0x3F));
      out[o++] = char(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = char(0x80 | (cp & 0x3F));
    }
  }
  out[o] = 0;
}

void* HookLabelLayout(void* widget, void* entity, const void* text, float* rect, float* placed) {
  ++g_inFlight;
  ItemLabel cur{};
  cur.entity = entity;
  wchar_t wide[96];
  const bool readable = text && ReadStdString(static_cast<const unsigned char*>(text), wide, 96);
  if (readable) ConvertLabelText(wide, cur.text, sizeof(cur.text), cur.color);
  const void* data = nullptr;
  if (readable && ReadPointer(static_cast<const char*>(widget) + 0x10, data) && data)
    ReadStdString(static_cast<const unsigned char*>(data) + 0x88, cur.style, sizeof(cur.style));
  t_currentLabel = readable ? &cur : nullptr;
  void* r = g_origLabelLayout(widget, entity, text, rect, placed);
  t_currentLabel = nullptr;
  ++g_itemLayouts;
  if (readable) ++g_itemReadable;
  if (cur.havePos) ++g_itemAnchored;
  if (g_itemDetailLogs < 3) {
    ++g_itemDetailLogs;
    size_t size = 0, cap = 0;
    if (text) {
      const void* sp = nullptr;
      ReadPointer(static_cast<const char*>(text) + 0x10, sp);
      size = reinterpret_cast<size_t>(sp);
      ReadPointer(static_cast<const char*>(text) + 0x18, sp);
      cap = reinterpret_cast<size_t>(sp);
    }
    Log("labels: item layout widget=%p entity=%p text=%p (size %zu cap %zu) readable=%d anchored=%d \"%s\" style \"%s\" "
        "rect %.0f %.0f %.0f %.0f",
        widget, entity, text, size, cap, int(readable), int(cur.havePos), cur.text, cur.style, rect ? rect[0] : -1.0f,
        rect ? rect[1] : -1.0f, rect ? rect[2] : -1.0f, rect ? rect[3] : -1.0f);
    // Raw text: the first 24 UTF-16 units as hex (what the label string really holds).
    char hex[24 * 5 + 1] = {};
    for (int i = 0; i < 24 && readable; ++i) snprintf(hex + i * 5, 6, "%04X ", unsigned(wide[i]) & 0xFFFF);
    Log("labels:   text units: %s", hex);
  }
  if (readable && cur.havePos && cur.text[0]) {
    cur.seenMs = ItemNowMs();
    {
      std::lock_guard lock(g_itemMutex);
      bool found = false;
      for (ItemLabel& l : g_items)
        if (l.entity == entity) {
          l = cur;
          found = true;
          break;
        }
      if (!found && g_items.size() < 256) g_items.push_back(cur);
      if (g_stylesSeen.size() < 32 &&
          std::find(g_stylesSeen.begin(), g_stylesSeen.end(), cur.style) == g_stylesSeen.end()) {
        g_stylesSeen.push_back(cur.style);
        Log("labels: item label style \"%s\" (e.g. \"%s\")", cur.style, cur.text);
      }
    }
    if (ItemLabels3dOn()) {
      // The game's flat label goes off-screen; the mod draws it in 3D.
      if (rect) rect[0] = rect[1] = -100000.0f;
      if (placed) placed[0] = placed[1] = -100000.0f;
      ++g_itemHidden;
    }
  }
  --g_inFlight;
  return r;
}

bool FromExe(void* ret) {
  HMODULE mod = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(ret), &mod) &&
         mod == g_exe;
}

enum Phase { kInWorldRender, kRenderThreadUi, kOtherThread, kPhases };
const char* const kPhaseNames[kPhases] = {"world render", "render thread (UI)", "other thread"};

struct Site {
  void* ret;
  int fn;  // 0 WorldCamera::Project, 1 Camera::Project
  int phase;
  unsigned long long count;
  unsigned long long drawsAfter = 0;  // UI phase: draws between this call and the next projection
  unsigned long long drawsSamples = 0;
};

// UI-phase bookkeeping (render thread only): which site projected last, and the draw count then.
int g_lastUiSite = -1;
unsigned long long g_lastUiDraws = 0;

// One-frame sample of what the UI projects.
struct Sample {
  void* ret;
  int fn;
  float world[3];
  float screen[2];
  unsigned long long draws;
};
std::vector<Sample> g_samples;
bool g_sampleThisFrame = false;
bool g_dumped = false;
std::mutex g_mutex;
std::vector<Site> g_sites;  // all-time counts
std::string g_report;       // last summary, for the UI

int Record(void* ret, int fn) {
  const int phase = t_worldRenderDepth > 0                       ? kInWorldRender
                    : GetCurrentThreadId() == g_renderThread.load() ? kRenderThreadUi
                                                                    : kOtherThread;
  std::lock_guard lock(g_mutex);
  int index = -1;
  for (size_t i = 0; i < g_sites.size(); ++i)
    if (g_sites[i].ret == ret && g_sites[i].fn == fn && g_sites[i].phase == phase) {
      index = int(i);
      break;
    }
  if (index < 0) {
    if (g_sites.size() >= 256) return -1;
    g_sites.push_back({ret, fn, phase, 0});
    index = int(g_sites.size()) - 1;
  }
  ++g_sites[index].count;
  if (phase == kRenderThreadUi) {
    const unsigned long long draws = d3dtrace::TotalDraws();
    if (g_lastUiSite >= 0) {
      g_sites[g_lastUiSite].drawsAfter += draws - g_lastUiDraws;
      ++g_sites[g_lastUiSite].drawsSamples;
    }
    g_lastUiSite = index;
    g_lastUiDraws = draws;
  }
  return phase;
}

// After the projection: keep a sample of what was projected (one frame every few seconds).
// `returned` is what the function returned: the hidden result pointer when the Vec2 goes through
// memory (then it equals `resultArg`), otherwise the two floats themselves, packed in RAX.
void Sampled(void* ret, int fn, int phase, const void* point, void* resultArg, void* returned) {
  if (phase != kRenderThreadUi || !g_sampleThisFrame) return;
  std::lock_guard lock(g_mutex);
  if (g_samples.size() >= 64) return;
  Sample smp{ret, fn, {}, {}, d3dtrace::TotalDraws()};
  // WorldVec3 = Region* then x, y, z; Vec3 = x, y, z. Vec2 = x, y.
  std::memcpy(smp.world, static_cast<const char*>(point) + (fn == 0 ? 8 : 0), sizeof(smp.world));
  if (returned == resultArg)
    std::memcpy(smp.screen, resultArg, sizeof(smp.screen));
  else
    std::memcpy(smp.screen, &returned, sizeof(smp.screen));
  g_samples.push_back(smp);
}

void* HookWorldProject(void* self, void* result, void* point, void* viewport) {
  ++g_inFlight;
  const int phase = Record(_ReturnAddress(), 0);
  void* r = g_origWorldProject(self, result, point, viewport);
  Sampled(_ReturnAddress(), 0, phase, point, result, r);
  if (t_currentLabel && g_exe &&
      _ReturnAddress() == reinterpret_cast<char*>(g_exe) + kLabelProjectReturnRva) {
    std::memcpy(t_currentLabel->pos, static_cast<const char*>(point) + 8, sizeof(t_currentLabel->pos));
    t_currentLabel->havePos = true;
  }
  float placed[2];
  if (phase == kRenderThreadUi && FromExe(_ReturnAddress()) &&
      _ReturnAddress() != reinterpret_cast<char*>(g_exe) + kLabelProjectReturnRva &&
      PlaceOnPanel(reinterpret_cast<const float*>(static_cast<const char*>(point) + 8), viewport, placed)) {
    if (r == result) {
      std::memcpy(result, placed, sizeof(placed));  // Vec2 returned through the hidden result pointer
    } else {
      std::memcpy(&r, placed, sizeof(placed));  // Vec2 returned packed in RAX
    }
  }
  --g_inFlight;
  return r;
}

void* HookCameraProject(void* self, void* result, void* point, void* viewport) {
  ++g_inFlight;
  const int phase = Record(_ReturnAddress(), 1);
  void* r = g_origCameraProject(self, result, point, viewport);
  Sampled(_ReturnAddress(), 1, phase, point, result, r);
  --g_inFlight;
  return r;
}

std::string Where(void* address) {
  HMODULE mod = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(address), &mod))
    return "?";
  char path[MAX_PATH];
  GetModuleFileNameA(mod, path, MAX_PATH);
  const char* name = strrchr(path, '\\');
  char buf[MAX_PATH + 32];
  snprintf(buf, sizeof(buf), "%s+0x%llX", name ? name + 1 : path,
           static_cast<unsigned long long>(static_cast<char*>(address) - reinterpret_cast<char*>(mod)));
  return buf;
}

}  // namespace

WorldRenderScope::WorldRenderScope() {
  g_renderThread = GetCurrentThreadId();
  ++t_worldRenderDepth;
}
WorldRenderScope::~WorldRenderScope() { --t_worldRenderDepth; }

void SetStereoFrame(Vec3 origin, Vec3 right, Vec3 up, Vec3 fwd, float scale) {
  g_frame = {g_frames, origin, right, up, fwd, scale > 0 ? scale : 1.0f};
}

bool Install(HMODULE engine) {
  bool ok = true;
  g_exe = GetModuleHandleW(nullptr);
  g_vpWidth = reinterpret_cast<ViewportGetIntFn>(GetProcAddress(engine, "?GetWidth@Viewport@GAME@@QEBAHXZ"));
  g_vpHeight = reinterpret_cast<ViewportGetIntFn>(GetProcAddress(engine, "?GetHeight@Viewport@GAME@@QEBAHXZ"));
  auto hook = [&](const char* name, void* detour, ProjectFn* orig) {
    void* target = reinterpret_cast<void*>(GetProcAddress(engine, name));
    if (!target || MH_CreateHook(target, detour, reinterpret_cast<void**>(orig)) != MH_OK) {
      Log("labels: could not hook %s", name);
      ok = false;
    }
  };
  hook(kWorldCameraProject, reinterpret_cast<void*>(&HookWorldProject), &g_origWorldProject);
  hook(kCameraProject, reinterpret_cast<void*>(&HookCameraProject), &g_origCameraProject);
  // Item label layout in Grim Dawn.exe: only hooked if the code is the version this was written for.
  auto* layout = reinterpret_cast<unsigned char*>(g_exe) + kLabelLayoutRva;
  if (!IsBadReadPtr(layout, sizeof(kLabelLayoutPrologue)) &&
      std::memcmp(layout, kLabelLayoutPrologue, sizeof(kLabelLayoutPrologue)) == 0 &&
      MH_CreateHook(layout, reinterpret_cast<void*>(&HookLabelLayout), reinterpret_cast<void**>(&g_origLabelLayout)) ==
          MH_OK) {
    Log("labels: item label layout hooked (Grim Dawn.exe+0x%llX)", static_cast<unsigned long long>(kLabelLayoutRva));
  } else {
    Log("labels: item label layout not found (different game version?): item names stay flat");
  }
  if (ok) Log("labels: projection hooks created");
  return ok;
}

// Saves the game's in-memory Grim Dawn.exe image (decrypted by Steam's DRM wrapper at start-up) for
// offline disassembly: offsets in the file are RVAs. Unreadable pages are left as zeros.
void DumpExeImage() {
#ifdef GDVR_DEV_LOG_DIR
  auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const size_t size = nt->OptionalHeader.SizeOfImage;
  std::vector<unsigned char> image(size, 0);
  for (size_t off = 0; off < size;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + off, &mbi, sizeof(mbi))) break;
    const size_t regionEnd = std::min(size, size_t(static_cast<const unsigned char*>(mbi.BaseAddress) - base) + mbi.RegionSize);
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_WRITECOPY;
    if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & PAGE_GUARD))
      std::memcpy(image.data() + off, base + off, regionEnd - off);
    off = regionEnd;
  }
  const std::wstring path = std::wstring(GDVR_DEV_LOG_DIR) + L"/grimdawn_mem.bin";
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
    fwrite(image.data(), 1, image.size(), f);
    fclose(f);
    Log("labels: saved the in-memory Grim Dawn.exe image (%zu bytes, base %p) to logs/grimdawn_mem.bin", size, base);
  }
#endif
}

void OnFramePresented() {
  static ULONGLONG lastLog = 0;
  if (!g_dumped && g_frames > 600) {
    g_dumped = true;
    DumpExeImage();
  }
  std::lock_guard lock(g_mutex);
  ++g_frames;
  g_lastUiSite = -1;  // draws after the frame's last projection belong to the rest of the UI
  if (g_sampleThisFrame) {
    g_sampleThisFrame = false;
    Log("labels: one frame of UI projections (site, world xyz -> screen xy, total draws):");
    for (const Sample& smp : g_samples)
      Log("  %s %-22s (%8.2f %8.2f %8.2f) -> (%7.1f %7.1f)  draws %llu", smp.fn ? "Cam  " : "World",
          Where(smp.ret).c_str(), smp.world[0], smp.world[1], smp.world[2], smp.screen[0], smp.screen[1], smp.draws);
    g_samples.clear();
  }
  if (GetTickCount64() - lastLog < 5000) return;
  lastLog = GetTickCount64();
  std::vector<Site> sites = g_sites;
  std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.count > b.count; });
  std::string report;
  Log("labels: projection callers over %llu frames (labels placed on the panel %llu, hidden behind %llu):", g_frames,
      g_placed.load(), g_hidden.load());
  Log("labels: item layouts %llu, readable %llu, anchored %llu, drawn in 3D %llu", g_itemLayouts.load(),
      g_itemReadable.load(), g_itemAnchored.load(), g_itemHidden.load());
  for (const Site& s : sites) {
    char line[256];
    snprintf(line, sizeof(line), "%s %-20s %-18s %8.2f/frame  draws after %.2f",
             s.fn ? "Camera::Project     " : "WorldCamera::Project", Where(s.ret).c_str(), kPhaseNames[s.phase],
             double(s.count) / double(g_frames),
             s.drawsSamples ? double(s.drawsAfter) / double(s.drawsSamples) : -1.0);
    Log("  %s", line);
    report += line;
    report += '\n';
  }
  g_report = report;
  g_sampleThisFrame = true;  // sample the next frame
}

namespace {
bool ItemLabels3dOn() { return xr::GetSettings().itemLabels3d && g_frame.frame != 0 && g_frames <= g_frame.frame + 2; }
}  // namespace

void DrawEye(const numbers::EyeView& v) {
  constexpr float kItemHeight = 0.42f;  // text height in game units
  constexpr float kItemLift = 0.7f;     // raised above the game's anchor so the name floats clear of the item
  constexpr double kStaleMs = 150.0;    // labels not laid out again for this long are gone
  const double now = ItemNowMs();
  std::vector<ItemLabel> live;
  {
    std::lock_guard lock(g_itemMutex);
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(),
                                 [&](const ItemLabel& l) { return now - l.seenMs > kStaleMs; }),
                  g_items.end());
    live = g_items;
  }
  if (live.empty() || !xr::GetSettings().itemLabels3d) return;
  std::vector<overlay::WorldText> items;
  items.reserve(live.size());
  for (const ItemLabel& l : live) {
    const Vec3 d = Vec3{l.pos[0], l.pos[1] + kItemLift, l.pos[2]} - v.origin;
    const float z = Dot(d, v.forward);
    if (z <= v.nearPlane) continue;
    const float ndcX = Dot(d, v.right) / (z * v.tanX);
    const float ndcY = Dot(d, v.up) / (z * v.tanY);
    if (std::fabs(ndcX) > 1.3f || std::fabs(ndcY) > 1.3f) continue;
    const float size = std::clamp(kItemHeight / (z * v.tanY) * v.height * 0.5f, 8.0f, 120.0f);
    items.push_back({(ndcX * 0.5f + 0.5f) * v.width, (0.5f - ndcY * 0.5f) * v.height, size,
                     l.color | 0xFF000000u, l.text, true});
  }
  overlay::DrawWorldText(items.data(), int(items.size()));
}

void DrawDebugUI() {
  ImGui::Text("Item labels: %llu laid out, %llu drawn in 3D instead", g_itemLayouts.load(), g_itemHidden.load());
  std::lock_guard lock(g_mutex);
  ImGui::TextUnformatted("Projection callers (world -> screen):");
  ImGui::TextUnformatted(g_report.empty() ? "  (none yet)" : g_report.c_str());
}

int InFlight() { return g_inFlight.load(); }

}  // namespace gdvr::labels
