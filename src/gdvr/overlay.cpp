#include "overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "engine_hooks.h"
#include "log.h"
#include "d3dtrace.h"
#include "mask.h"
#include "sky.h"
#include "perf.h"
#include "pointer.h"
#include "xr.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace gdvr::overlay {

// Defined below; called from the Present hook on the render thread.
void Teardown();

namespace {

std::atomic<unsigned long long> g_lastPresent{0};  // GetTickCount64() of the last frame shown (freeze watchdog)

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

constexpr int kPresentIndex = 8;         // IDXGISwapChain vtable slots
constexpr int kResizeBuffersIndex = 13;
constexpr int kToggleKey = VK_F8;

PresentFn g_origPresent = nullptr;
ResizeBuffersFn g_origResizeBuffers = nullptr;

// Capture tools (OBS game capture, Steam recording, overlays) hook Present's code too, and whichever hooked
// last runs first: one that hooked after us copied the frame before the desktop mirror was drawn into it.
// So the game's swap chain also gets its vtable entry pointed at us, which runs before any hook on the
// function itself; the inline hook stays as the way in until then, and passes through while we're inside.
void** g_vtable = nullptr;
PresentFn g_vtableNext = nullptr;
bool g_inFrontPresent = false;

std::atomic<int> g_inFlight{0};
std::atomic<bool> g_teardownRequested{false};
std::atomic<bool> g_tornDown{false};
HANDLE g_teardownDone = nullptr;
bool g_imguiReady = false;
bool g_menuVisible = true;
std::atomic<bool> g_toggleRequested{false};  // from the controllers (both sticks held)
bool g_iconShown = false;   // the settings gear (opens / closes the window) is drawn this frame (in VR)
bool g_uiBroken = false;    // ImGui's device objects are missing: no UI at all
HWND g_window = nullptr;
WNDPROC g_origWndProc = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
unsigned g_bbWidth = 0, g_bbHeight = 0;
IDXGISwapChain* g_swapChain = nullptr;  // the game's, remembered at Present
ImFont* g_numberFont = nullptr;  // large bold font for text drawn into the eye images
ImDrawList* g_worldList = nullptr;

struct InFlightGuard {
  InFlightGuard() { ++g_inFlight; }
  ~InFlightGuard() { --g_inFlight; }
};

LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (g_imguiReady && (g_menuVisible || g_iconShown) && !g_tornDown) {
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
    const ImGuiIO& io = ImGui::GetIO();
    const bool mouseMsg = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
    const bool keyMsg = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
    if ((mouseMsg && io.WantCaptureMouse) || (keyMsg && io.WantCaptureKeyboard)) return 0;
  }
  return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

void CreateRtv(IDXGISwapChain* swapChain) {
  ID3D11Texture2D* backBuffer = nullptr;
  if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    D3D11_TEXTURE2D_DESC d;
    backBuffer->GetDesc(&d);
    g_bbWidth = d.Width;
    g_bbHeight = d.Height;
    backBuffer->Release();
  }
}

bool InitImGui(IDXGISwapChain* swapChain) {
  if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device)))) return false;
  g_device->GetImmediateContext(&g_context);

  DXGI_SWAP_CHAIN_DESC desc;
  swapChain->GetDesc(&desc);
  g_window = desc.OutputWindow;

  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  // The UI keeps the default font (added first); a large bold font is added for world text.
  ImGui::GetIO().Fonts->AddFontDefault();
  char fontPath[MAX_PATH];
  if (GetWindowsDirectoryA(fontPath, MAX_PATH)) {
    strcat_s(fontPath, "\\Fonts\\segoeuib.ttf");
    if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES)
      g_numberFont = ImGui::GetIO().Fonts->AddFontFromFileTTF(fontPath, 64.0f);
  }
  ImGui::StyleColorsDark();
  // Scale the UI with the window height (the default font is unreadable at 4K).
  const float uiScale = std::max(1.0f, float(desc.BufferDesc.Height) / 1080.0f);
  ImGui::GetIO().FontGlobalScale = uiScale;
  ImGui::GetStyle().ScaleAllSizes(uiScale);
  ImGui_ImplWin32_Init(g_window);
  ImGui_ImplDX11_Init(g_device, g_context);
  g_origWndProc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookWndProc)));

  Log("ImGui initialised: window=%p %ux%u", g_window, desc.BufferDesc.Width, desc.BufferDesc.Height);
  return true;
}

HRESULT STDMETHODCALLTYPE FrontPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

void PatchVtable(IDXGISwapChain* swapChain) {
  static bool tried = false;
  if (tried) return;
  tried = true;
  void** vtable = *reinterpret_cast<void***>(swapChain);
  DWORD old;
  if (!VirtualProtect(&vtable[kPresentIndex], sizeof(void*), PAGE_READWRITE, &old)) {
    Log("Present vtable entry not writable; capture tools may record the frame without the mirror");
    return;
  }
  g_vtableNext = reinterpret_cast<PresentFn>(vtable[kPresentIndex]);
  InterlockedExchangePointer(&vtable[kPresentIndex], reinterpret_cast<void*>(&FrontPresent));
  VirtualProtect(&vtable[kPresentIndex], sizeof(void*), old, &old);
  g_vtable = vtable;
  Log("Present vtable entry hooked (ahead of capture tools)");
}

void UnpatchVtable() {
  if (!g_vtable) return;
  DWORD old;
  if (g_vtable[kPresentIndex] == reinterpret_cast<void*>(&FrontPresent) &&
      VirtualProtect(&g_vtable[kPresentIndex], sizeof(void*), PAGE_READWRITE, &old)) {
    InterlockedExchangePointer(&g_vtable[kPresentIndex], reinterpret_cast<void*>(g_vtableNext));
    VirtualProtect(&g_vtable[kPresentIndex], sizeof(void*), old, &old);
  }
  g_vtable = nullptr;
}

// The settings gear (opens and closes the window): only in VR with the laser, and not over a game menu.
bool ShowSettingsIcon() {
  const xr::Settings& s = xr::GetSettings();
  return xr::StereoFrameActive() && s.pointer && !s.pureController && !xr::HudMenuOpen();
}

// A gear just right of the bottom bar on the off hand (or on the floating panel's left edge when the bar isn't
// on the hand); true when clicked (laser trigger or mouse).
bool DrawSettingsIcon() {
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 screen = io.DisplaySize;
  float bar[4];
  const bool onHand = xr::HandBarShown(bar) && bar[3] > bar[1];
  float button, size;  // the clickable square (a margin around the gear) and the gear's backing, in pixels
  ImVec2 at;
  if (onHand) {
    button = (bar[3] - bar[1]) * screen.y * 0.425f;
    size = button * 0.8f;
    at = ImVec2(bar[2] * screen.x + 0.015f * screen.x, ((bar[1] + bar[3]) * 0.5f) * screen.y - button * 0.5f);
  } else {
    button = screen.y * 0.04f;
    size = button * 0.8f;
    at = ImVec2(screen.x * 0.012f, screen.y * 0.40f);
  }
  ImGui::SetNextWindowPos(at, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_AlwaysAutoResize;
  ImGui::Begin("###gdvr_icon", nullptr, kFlags);
  const ImVec2 b = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("open", ImVec2(button, button));
  const bool hovered = ImGui::IsItemHovered();
  if (screen.x > 0 && screen.y > 0)
    xr::SetSettingsIconRect(b.x / screen.x, b.y / screen.y, (b.x + button) / screen.x, (b.y + button) / screen.y, onHand);
  const ImVec2 p(b.x + (button - size) * 0.5f, b.y + (button - size) * 0.5f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
  const ImU32 back = hovered ? IM_COL32(70, 60, 40, 230) : IM_COL32(20, 18, 14, 190);
  const ImU32 fore = hovered ? IM_COL32(255, 220, 140, 255) : IM_COL32(215, 190, 140, 235);
  dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), back, size * 0.22f);
  dl->AddRect(p, ImVec2(p.x + size, p.y + size), fore, size * 0.22f, 0, size * 0.03f);
  // Gear: eight teeth around a ring.
  const float rOuter = size * 0.34f, rRing = size * 0.24f, rHole = size * 0.10f;
  for (int k = 0; k < 8; ++k) {
    const float a = k * 3.14159265f / 4.0f, ca = std::cos(a), sa = std::sin(a);
    dl->AddLine(ImVec2(c.x + ca * rRing, c.y + sa * rRing), ImVec2(c.x + ca * rOuter, c.y + sa * rOuter), fore,
                size * 0.11f);
  }
  dl->AddCircleFilled(c, rRing, fore, 24);
  dl->AddCircleFilled(c, rHole, back, 16);
  ImGui::End();
  ImGui::PopStyleVar(2);
  return clicked;
}

// One frame of the mod's work, then `next` presents it.
HRESULT Frame(PresentFn next, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
  if (g_tornDown) return next(swapChain, syncInterval, flags);
  if (g_teardownRequested) {
    Teardown();
    SetEvent(g_teardownDone);
    return next(swapChain, syncInterval, flags);
  }

  PatchVtable(swapChain);
  g_swapChain = swapChain;
  if (!g_imguiReady) {
    g_imguiReady = InitImGui(swapChain);
    if (g_context) {
      mask::InstallHook(g_context);
      d3dtrace::InstallHooks(g_context);
    }
  }

  static bool keyWasDown = false;
  const bool keyDown = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
  if ((keyDown && !keyWasDown) || g_toggleRequested.exchange(false)) g_menuVisible = !g_menuVisible;
  keyWasDown = keyDown;

  engine::OnFramePresented();

  // With the window hidden in VR, a small icon on the HUD panel opens it again with the laser.
  g_iconShown = ShowSettingsIcon();
  const bool drawUi = g_menuVisible || g_iconShown;
  if (g_imguiReady && drawUi && !g_uiBroken) {
    if (!g_rtv) CreateRtv(swapChain);
    ImGui_ImplDX11_NewFrame();
    // If the backend failed to create its device objects the font atlas is empty and NewFrame
    // would dereference a null font (seen once after a hot reload): skip the UI instead.
    if (ImGui::GetIO().Fonts->Fonts.Size == 0) {
      Log("ImGui device objects missing (font atlas empty); debug window disabled");
      g_uiBroken = true;
      g_menuVisible = g_iconShown = false;
    }
  }
  if (g_imguiReady && drawUi && !g_uiBroken) {
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().MouseDrawCursor = g_menuVisible;
    {
      // The controller laser on the window works it like a mouse (after the Win32 backend, so it wins).
      ImGuiIO& io = ImGui::GetIO();
      static bool laserWas = false, downWas = false;
      float u, v, wheel;
      bool down;
      if (pointer::LaserOnDebugWindow(u, v, down, wheel)) {
        io.AddMousePosEvent(u * io.DisplaySize.x, v * io.DisplaySize.y);
        if (down != downWas) io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        if (wheel != 0) io.AddMouseWheelEvent(0, wheel);
        laserWas = true;
        downWas = down;
      } else if (laserWas) {
        if (downWas) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        laserWas = downWas = false;
      }
    }
    ImGui::NewFrame();
    if (g_menuVisible) {
      bool open = true;
      engine::DrawDebugUI(&open);  // its X closes it
      if (!open) g_menuVisible = false;
    }
    if (g_iconShown && DrawSettingsIcon()) g_menuVisible = !g_menuVisible;
    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  }

  xr::OnPresent(swapChain);
  if (g_device) perf::OnPresent(g_device, g_context, xr::LastWaitMs());
  g_lastPresent = GetTickCount64();
  return next(swapChain, syncInterval, flags);
}

// Through the vtable: the frame's work, then on to Present's code (capture tools, then our inline hook).
HRESULT STDMETHODCALLTYPE FrontPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
  InFlightGuard guard;
  g_inFrontPresent = true;
  const HRESULT hr = Frame(g_vtableNext, swapChain, syncInterval, flags);
  g_inFrontPresent = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
  InFlightGuard guard;
  if (g_inFrontPresent) return g_origPresent(swapChain, syncInterval, flags);
  return Frame(g_origPresent, swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags) {
  InFlightGuard guard;
  if (g_rtv) {
    g_rtv->Release();
    g_rtv = nullptr;
  }
  return g_origResizeBuffers(swapChain, count, width, height, format, flags);
}

}  // namespace

void DrawWorld(const WorldText* texts, int textCount, const Polyline* lines, int lineCount) {
  if (!g_imguiReady || g_tornDown || (textCount <= 0 && lineCount <= 0) || !g_context) return;
  if (!g_rtv) {
    if (!g_swapChain) return;
    CreateRtv(g_swapChain);
    if (!g_rtv) return;
  }
  ImGui_ImplDX11_NewFrame();  // creates the backend's device objects if the menu was never shown
  ImGuiIO& io = ImGui::GetIO();
  if (io.Fonts->Fonts.Size == 0) return;
  ImFont* font = g_numberFont ? g_numberFont : io.Fonts->Fonts[0];
  if (!g_worldList) g_worldList = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
  ImDrawList* list = g_worldList;
  list->_ResetForNewFrame();
  list->Flags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
  list->PushTextureID(io.Fonts->TexID);
  list->PushClipRect(ImVec2(0, 0), ImVec2(float(g_bbWidth), float(g_bbHeight)));
  for (int i = 0; i < lineCount; ++i) {
    const Polyline& l = lines[i];
    if (l.points < 2) continue;
    list->AddPolyline(reinterpret_cast<const ImVec2*>(l.xy), l.points, l.color,
                      l.closed ? ImDrawFlags_Closed : ImDrawFlags_None, l.thickness);
  }
  for (int i = 0; i < textCount; ++i) {
    const WorldText& t = texts[i];
    const ImVec2 sz = font->CalcTextSizeA(t.size, FLT_MAX, 0, t.text);
    const ImVec2 pos(t.x - sz.x * 0.5f, t.y - sz.y * 0.5f);
    const unsigned alpha = t.color >> 24;
    if (t.box) {
      const float pad = t.size * 0.25f;
      list->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.4f), ImVec2(pos.x + sz.x + pad, pos.y + sz.y + pad * 0.4f),
                          IM_COL32(0, 0, 0, alpha * 170 / 255), t.size * 0.15f);
    } else {
      const float o = std::max(1.0f, t.size * 0.06f);  // dark outline for readability
      for (int k = 0; k < 4; ++k)
        list->AddText(font, t.size, ImVec2(pos.x + (k & 1 ? o : -o), pos.y + (k & 2 ? o : -o)), (alpha << 24), t.text);
    }
    list->AddText(font, t.size, pos, t.color, t.text);
  }
  list->PopClipRect();
  list->PopTextureID();

  ImDrawData data;
  data.Valid = true;
  data.DisplayPos = ImVec2(0, 0);
  data.DisplaySize = ImVec2(float(g_bbWidth), float(g_bbHeight));
  data.FramebufferScale = ImVec2(1, 1);
  data.AddDrawList(list);

  ID3D11RenderTargetView* oldRtv = nullptr;
  ID3D11DepthStencilView* oldDsv = nullptr;
  g_context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
  g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
  ImGui_ImplDX11_RenderDrawData(&data);  // saves and restores the rest of the pipeline state
  g_context->OMSetRenderTargets(1, &oldRtv, oldDsv);
  if (oldRtv) oldRtv->Release();
  if (oldDsv) oldDsv->Release();
}

HWND GameWindow() { return g_window; }

void ToggleMenu() { g_toggleRequested = true; }

unsigned long long LastPresentTick() { return g_lastPresent.load(); }

void DrawWorldText(const WorldText* items, int count) { DrawWorld(items, count, nullptr, 0); }

bool Install() {
  // A throwaway device + swapchain on a hidden window, just to read the vtable.
  WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr)};
  wc.lpszClassName = L"gdvr_dummy";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                            wc.hInstance, nullptr);

  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 1;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;

  IDXGISwapChain* swapChain = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &swapChain, &device, nullptr, &context);
  if (FAILED(hr)) {
    Log("Dummy D3D11CreateDeviceAndSwapChain failed: 0x%08X", unsigned(hr));
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return false;
  }

  void** vtable = *reinterpret_cast<void***>(swapChain);
  void* present = vtable[kPresentIndex];
  void* resize = vtable[kResizeBuffersIndex];
  swapChain->Release();
  context->Release();
  device->Release();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);

  if (MH_CreateHook(present, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_origPresent)) != MH_OK ||
      MH_CreateHook(resize, reinterpret_cast<void*>(&HookResizeBuffers),
                    reinterpret_cast<void**>(&g_origResizeBuffers)) != MH_OK) {
    Log("Failed to hook swapchain");
    return false;
  }
  Log("Swapchain hooks created (Present @ %p)", present);
  return true;
}

// Releases everything that uses the D3D context or OpenXR, plus ImGui and the window hook.
void Teardown() {
  if (g_tornDown.exchange(true)) return;
  UnpatchVtable();
  xr::Shutdown();
  mask::Shutdown();
  sky::Shutdown();
  perf::Shutdown();
  if (g_origWndProc) SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
  if (g_imguiReady) {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imguiReady = false;
  }
  if (g_worldList) {
    IM_DELETE(g_worldList);
    g_worldList = nullptr;
  }
  g_numberFont = nullptr;
  if (g_rtv) g_rtv->Release();
  if (g_context) g_context->Release();
  if (g_device) g_device->Release();
  g_rtv = nullptr;
  g_context = nullptr;
  g_device = nullptr;
  Log("teardown complete");
}

bool RequestTeardown(unsigned timeoutMs) {
  if (g_tornDown) return true;
  g_teardownDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_teardownRequested = true;
  const bool done = WaitForSingleObject(g_teardownDone, timeoutMs) == WAIT_OBJECT_0;
  if (!done) g_teardownRequested = false;  // not presenting: caller tears down instead
  return done;
}

void TeardownNow() { Teardown(); }

int InFlight() { return g_inFlight.load(); }

}  // namespace gdvr::overlay
