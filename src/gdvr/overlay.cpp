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

#include "engine_hooks.h"
#include "log.h"
#include "d3dtrace.h"
#include "mask.h"
#include "perf.h"
#include "xr.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace gdvr::overlay {

// Defined below; called from the Present hook on the render thread.
void Teardown();

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

constexpr int kPresentIndex = 8;         // IDXGISwapChain vtable slots
constexpr int kResizeBuffersIndex = 13;
constexpr int kToggleKey = VK_F8;

PresentFn g_origPresent = nullptr;
ResizeBuffersFn g_origResizeBuffers = nullptr;

std::atomic<int> g_inFlight{0};
std::atomic<bool> g_teardownRequested{false};
std::atomic<bool> g_tornDown{false};
HANDLE g_teardownDone = nullptr;
bool g_imguiReady = false;
bool g_menuVisible = true;
HWND g_window = nullptr;
WNDPROC g_origWndProc = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

struct InFlightGuard {
  InFlightGuard() { ++g_inFlight; }
  ~InFlightGuard() { --g_inFlight; }
};

LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (g_imguiReady && g_menuVisible && !g_tornDown) {
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

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
  InFlightGuard guard;
  if (g_tornDown) return g_origPresent(swapChain, syncInterval, flags);
  if (g_teardownRequested) {
    Teardown();
    SetEvent(g_teardownDone);
    return g_origPresent(swapChain, syncInterval, flags);
  }

  if (!g_imguiReady) {
    g_imguiReady = InitImGui(swapChain);
    if (g_context) {
      mask::InstallHook(g_context);
      d3dtrace::InstallHooks(g_context);
    }
  }

  static bool keyWasDown = false;
  const bool keyDown = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
  if (keyDown && !keyWasDown) g_menuVisible = !g_menuVisible;
  keyWasDown = keyDown;

  engine::OnFramePresented();

  if (g_imguiReady && g_menuVisible) {
    if (!g_rtv) CreateRtv(swapChain);
    ImGui_ImplDX11_NewFrame();
    // If the backend failed to create its device objects the font atlas is empty and NewFrame
    // would dereference a null font (seen once after a hot reload): skip the UI instead.
    if (ImGui::GetIO().Fonts->Fonts.Size == 0) {
      static bool warned = false;
      if (!warned) Log("ImGui device objects missing (font atlas empty); debug window disabled");
      warned = true;
      g_menuVisible = false;
    }
  }
  if (g_imguiReady && g_menuVisible) {
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().MouseDrawCursor = true;
    ImGui::NewFrame();
    engine::DrawDebugUI();
    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  }

  xr::OnPresent(swapChain);
  if (g_device) perf::OnPresent(g_device, g_context, xr::LastWaitMs());
  return g_origPresent(swapChain, syncInterval, flags);
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
  xr::Shutdown();
  mask::Shutdown();
  perf::Shutdown();
  if (g_origWndProc) SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
  if (g_imguiReady) {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imguiReady = false;
  }
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
