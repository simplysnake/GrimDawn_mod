#include "xr.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "d3dtrace.h"
#include "input.h"
#include "log.h"

namespace gdvr::xr {

namespace {

constexpr double kInitRetryMs = 5000.0;

struct Swapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  UINT width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  std::vector<XrSwapchainImageD3D11KHR> images;
};

Settings g_settings;
std::atomic<bool> g_recenterRequested{true};
std::mutex g_mutex;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_gameSwapChain = nullptr;

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_localSpace = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
double g_nextInitAttempt = 0;
std::string g_status = "not started";
std::vector<int64_t> g_formats;

Swapchain g_eyeChains[2];
Swapchain g_screenChain;
Swapchain g_hudChain;
bool g_hudPrepared = false;              // back buffer cleared for the HUD this frame
ID3D11RenderTargetView* g_bbRtv = nullptr;
ID3D11Texture2D* g_bbRtvTex = nullptr;   // back buffer g_bbRtv was made for (not owned)

bool g_frameBegun = false;
XrFrameState g_frameState{XR_TYPE_FRAME_STATE};
XrView g_views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool g_viewsValid = false;
bool g_haveRef = false;
Pose g_ref;               // recentred origin (yaw only + head position), in LOCAL space
Pose g_eyeRel[2];         // eye poses relative to g_ref
bool g_eyeCaptured[2] = {};
float g_eyeTan[2][2] = {};
uint64_t g_framesSubmitted = 0;
double g_lastWaitMs = 0;
std::atomic<int> g_dumpEyes{0};  // F11: bit per eye still to dump

double NowMs() { return double(GetTickCount64()); }

Pose ToPose(const XrPosef& p) {
  return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
          {p.position.x, p.position.y, p.position.z}};
}
XrPosef ToXr(const Pose& p) {
  XrPosef out;
  out.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
  out.position = {p.position.x, p.position.y, p.position.z};
  return out;
}

bool Check(XrResult r, const char* what) {
  if (XR_SUCCEEDED(r)) return true;
  char buf[XR_MAX_RESULT_STRING_SIZE] = "";
  if (g_instance) xrResultToString(g_instance, r, buf);
  g_status = std::string(what) + " failed: " + (buf[0] ? buf : std::to_string(int(r)));
  Log("XR %s", g_status.c_str());
  return false;
}

void DestroySwapchain(Swapchain& sc) {
  if (sc.handle) xrDestroySwapchain(sc.handle);
  sc = Swapchain{};
}

void DestroyAll() {
  input::DestroyActions();
  DestroySwapchain(g_eyeChains[0]);
  DestroySwapchain(g_eyeChains[1]);
  DestroySwapchain(g_screenChain);
  DestroySwapchain(g_hudChain);
  if (g_viewSpace) xrDestroySpace(g_viewSpace);
  if (g_localSpace) xrDestroySpace(g_localSpace);
  if (g_session) xrDestroySession(g_session);
  if (g_instance) xrDestroyInstance(g_instance);
  g_viewSpace = g_localSpace = XR_NULL_HANDLE;
  g_session = XR_NULL_HANDLE;
  g_instance = XR_NULL_HANDLE;
  g_system = XR_NULL_SYSTEM_ID;
  g_running = false;
  g_frameBegun = false;
  g_viewsValid = false;
  g_haveRef = false;
  g_state = XR_SESSION_STATE_UNKNOWN;
  g_formats.clear();
}

bool Init() {
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy_s(ci.applicationInfo.applicationName, "GrimDawnVR");
  strcpy_s(ci.applicationInfo.engineName, "gdvr");
  ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = extensions;
  if (!Check(xrCreateInstance(&ci, &g_instance), "xrCreateInstance")) return false;

  XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
  si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if (!Check(xrGetSystem(g_instance, &si, &g_system), "xrGetSystem (headset connected? SteamVR running?)"))
    return false;

  PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
  xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements));
  XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  if (!getRequirements || !Check(getRequirements(g_instance, g_system, &req), "xrGetD3D11GraphicsRequirementsKHR"))
    return false;

  // The headset must be driven by the same GPU the game renders on.
  IDXGIDevice* dxgiDevice = nullptr;
  IDXGIAdapter* adapter = nullptr;
  DXGI_ADAPTER_DESC adesc{};
  if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
    adapter->GetDesc(&adesc);
  if (adapter) adapter->Release();
  if (dxgiDevice) dxgiDevice->Release();
  if (std::memcmp(&adesc.AdapterLuid, &req.adapterLuid, sizeof(LUID)) != 0) {
    g_status = "game GPU differs from the headset's GPU";
    Log("XR %s", g_status.c_str());
    return false;
  }

  XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
  binding.device = g_device;
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &binding;
  sci.systemId = g_system;
  if (!Check(xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession")) return false;

  XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rs.poseInReferenceSpace.orientation.w = 1;
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  if (!Check(xrCreateReferenceSpace(g_session, &rs, &g_localSpace), "xrCreateReferenceSpace(LOCAL)")) return false;
  rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  if (!Check(xrCreateReferenceSpace(g_session, &rs, &g_viewSpace), "xrCreateReferenceSpace(VIEW)")) return false;

  uint32_t count = 0;
  xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr);
  g_formats.resize(count);
  xrEnumerateSwapchainFormats(g_session, count, &count, g_formats.data());

  input::CreateActions(g_instance, g_session);

  XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
  xrGetSystemProperties(g_instance, g_system, &props);
  g_status = std::string("session created: ") + props.systemName;
  Log("XR %s (GPU %ls)", g_status.c_str(), adesc.Description);
  return true;
}

// sRGB variant of a back-buffer format, so the runtime treats the (already gamma-encoded) game image correctly.
DXGI_FORMAT SrgbOf(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return f;
  }
}

// (Re)creates `sc` to match the back buffer so frames can be copied with CopyResource.
bool EnsureSwapchain(Swapchain& sc, const D3D11_TEXTURE2D_DESC& bb) {
  if (sc.handle && sc.width == bb.Width && sc.height == bb.Height) return true;
  DestroySwapchain(sc);

  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  for (DXGI_FORMAT candidate : {SrgbOf(bb.Format), bb.Format})
    if (std::find(g_formats.begin(), g_formats.end(), int64_t(candidate)) != g_formats.end()) {
      format = candidate;
      break;
    }
  if (format == DXGI_FORMAT_UNKNOWN) {
    g_status = "runtime has no swapchain format matching back buffer format " + std::to_string(int(bb.Format));
    Log("XR %s", g_status.c_str());
    return false;
  }

  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  ci.format = int64_t(format);
  ci.sampleCount = 1;
  ci.width = bb.Width;
  ci.height = bb.Height;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  if (!Check(xrCreateSwapchain(g_session, &ci, &sc.handle), "xrCreateSwapchain")) return false;

  uint32_t count = 0;
  xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
  sc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  xrEnumerateSwapchainImages(sc.handle, count, &count,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data()));
  sc.width = bb.Width;
  sc.height = bb.Height;
  sc.format = format;
  Log("XR swapchain %ux%u format %d (%u images)", sc.width, sc.height, int(format), count);
  return true;
}

// Copies the game's current back buffer into the next image of `sc`.
bool CaptureBackBuffer(Swapchain& sc) {
  if (!g_gameSwapChain) return false;
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g_gameSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  D3D11_TEXTURE2D_DESC desc;
  bb->GetDesc(&desc);

  bool ok = false;
  if (desc.SampleDesc.Count != 1) {
    g_status = "multisampled back buffer is not supported yet (turn off MSAA)";
  } else if (EnsureSwapchain(sc, desc)) {
    uint32_t index = 0;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (Check(xrAcquireSwapchainImage(sc.handle, nullptr, &index), "xrAcquireSwapchainImage") &&
        Check(xrWaitSwapchainImage(sc.handle, &wait), "xrWaitSwapchainImage")) {
      g_context->CopyResource(sc.images[index].texture, bb);
      ok = Check(xrReleaseSwapchainImage(sc.handle, nullptr), "xrReleaseSwapchainImage");
    }
  }
  bb->Release();
  return ok;
}

void PollEvents() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto& e = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
      g_state = e.state;
      Log("XR session state %d", int(g_state));
      if (g_state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        g_running = Check(xrBeginSession(g_session, &bi), "xrBeginSession");
        if (g_running) g_status = "running";
      } else if (g_state == XR_SESSION_STATE_STOPPING) {
        g_running = false;
        g_frameBegun = false;
        xrEndSession(g_session);
        g_status = "stopped by runtime";
      } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
        DestroyAll();
        g_status = "session lost; will retry";
        g_nextInitAttempt = NowMs() + kInitRetryMs;
        return;
      }
    } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
      // SteamVR "Reset View" (or similar): recentre on the new origin.
      g_recenterRequested = true;
    } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
      input::OnInteractionProfileChanged(g_instance, g_session);
    } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      DestroyAll();
      g_status = "runtime went away; will retry";
      g_nextInitAttempt = NowMs() + kInitRetryMs;
      return;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

// Ends the frame begun by the previous OnPresent, submitting whatever was captured for it.
void EndFrame() {
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                         {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  std::vector<XrCompositionLayerBaseHeader*> layers;

  if (g_frameState.shouldRender && g_haveRef) {
    const bool stereo = g_settings.mode == Mode::Stereo && g_viewsValid && g_eyeCaptured[0] && g_eyeCaptured[1];
    if (stereo) {
      for (int i = 0; i < 2; ++i) {
        pv[i].pose = g_views[i].pose;
        pv[i].fov = {-std::atan(g_eyeTan[i][0]), std::atan(g_eyeTan[i][0]), std::atan(g_eyeTan[i][1]),
                     -std::atan(g_eyeTan[i][1])};
        pv[i].subImage.swapchain = g_eyeChains[i].handle;
        pv[i].subImage.imageRect = {{0, 0}, {int32_t(g_eyeChains[i].width), int32_t(g_eyeChains[i].height)}};
      }
      proj.space = g_localSpace;
      proj.viewCount = 2;
      proj.views = pv;
      layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj));

      // HUD: the game drew only its UI onto the cleared back buffer; show it on a floating panel.
      if (g_settings.hud && g_hudPrepared) {
        mask::FixHudAlpha(g_gameSwapChain);
        if (CaptureBackBuffer(g_hudChain)) {
          const float tilt = -g_settings.hudTiltDeg * 3.14159265f / 180.0f;
          const Quat pitch{std::sin(tilt * 0.5f), 0, 0, std::cos(tilt * 0.5f)};
          const Quat yaw = g_ref.orientation;
          hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
          hudQuad.space = g_localSpace;
          hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
          hudQuad.subImage.swapchain = g_hudChain.handle;
          hudQuad.subImage.imageRect = {{0, 0}, {int32_t(g_hudChain.width), int32_t(g_hudChain.height)}};
          hudQuad.pose = ToXr({yaw * pitch, g_ref.position + Rotate(yaw, {0, -g_settings.hudDown, -g_settings.hudDistance})});
          hudQuad.size = {g_settings.hudWidth,
                          g_settings.hudWidth * float(g_hudChain.height) / float(std::max(1u, g_hudChain.width))};
          layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&hudQuad));
        }
      }
    } else if (g_settings.mode != Mode::Off && CaptureBackBuffer(g_screenChain)) {
      // Screen mode, or stereo with nothing rendered this frame (menus, loading): flat screen.
      const Quat yaw = g_ref.orientation;
      quad.space = g_localSpace;
      quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      quad.subImage.swapchain = g_screenChain.handle;
      quad.subImage.imageRect = {{0, 0}, {int32_t(g_screenChain.width), int32_t(g_screenChain.height)}};
      quad.pose = ToXr({yaw, g_ref.position + Rotate(yaw, {0, 0, -g_settings.screenDistance})});
      quad.size = {g_settings.screenWidth,
                   g_settings.screenWidth * float(g_screenChain.height) / float(std::max(1u, g_screenChain.width))};
      layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
    }
  }

  XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
  ei.displayTime = g_frameState.predictedDisplayTime;
  ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  ei.layerCount = uint32_t(layers.size());
  ei.layers = layers.data();
  Check(xrEndFrame(g_session, &ei), "xrEndFrame");
  g_frameBegun = false;
  if (!layers.empty()) ++g_framesSubmitted;
}

void BeginFrame() {
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  g_frameState = {XR_TYPE_FRAME_STATE};
  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  const bool waited = Check(xrWaitFrame(g_session, &wi, &g_frameState), "xrWaitFrame");
  QueryPerformanceCounter(&t1);
  g_lastWaitMs = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
  if (!waited) return;
  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  if (!Check(xrBeginFrame(g_session, &bi), "xrBeginFrame")) return;
  g_frameBegun = true;
  g_eyeCaptured[0] = g_eyeCaptured[1] = false;
  g_hudPrepared = false;

  // Head pose (for recentring) and eye views for this frame's predicted display time.
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  xrLocateSpace(g_viewSpace, g_localSpace, g_frameState.predictedDisplayTime, &head);
  const bool headValid = (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                         (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
  if (headValid && (g_recenterRequested.exchange(false) || !g_haveRef)) {
    const Pose h = ToPose(head.pose);
    g_ref = {YawQuat(YawOf(h.orientation)), h.position};
    g_haveRef = true;
    Log("XR recentred");
  }

  XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
  li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  li.displayTime = g_frameState.predictedDisplayTime;
  li.space = g_localSpace;
  XrViewState vs{XR_TYPE_VIEW_STATE};
  uint32_t n = 0;
  g_views[0] = g_views[1] = {XR_TYPE_VIEW};
  g_viewsValid = XR_SUCCEEDED(xrLocateViews(g_session, &li, &vs, 2, &n, g_views)) && n == 2 &&
                 (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                 (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
  if (g_viewsValid && g_haveRef)
    for (int i = 0; i < 2; ++i) g_eyeRel[i] = Relative(g_ref, ToPose(g_views[i].pose));

  // Controllers -> virtual gamepad (read by the game through XInputGetState).
  if (input::Sync(g_session, g_state == XR_SESSION_STATE_FOCUSED)) g_recenterRequested = true;
}

}  // namespace

Settings& GetSettings() { return g_settings; }
void RequestRecenter() { g_recenterRequested = true; }

void OnPresent(IDXGISwapChain* swapChain) {
  std::lock_guard lock(g_mutex);
  g_gameSwapChain = swapChain;
  if (!g_device) {
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device)))) return;
    g_device->GetImmediateContext(&g_context);
  }

  // Hotkeys: F7 cycles the mask (on -> distance rings -> off), F9 cycles Off -> Screen -> Stereo,
  // F10 recentres.
  static bool f7Was = false, f9Was = false, f10Was = false, f11Was = false;
  const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
  if (f11 && !f11Was) g_dumpEyes = 3;
  f11Was = f11;
  const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
  if (f7 && !f7Was) {
    if (g_settings.maskEnabled && !g_settings.maskDebug) {
      g_settings.maskDebug = true;
    } else if (g_settings.maskEnabled) {
      g_settings.maskEnabled = false;
      g_settings.maskDebug = false;
    } else {
      g_settings.maskEnabled = true;
    }
    Log("mask -> %s", !g_settings.maskEnabled ? "off" : g_settings.maskDebug ? "debug rings" : "on");
  }
  f7Was = f7;
  const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
  const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  if (f9 && !f9Was) {
    g_settings.mode = Mode((int(g_settings.mode) + 1) % 3);
    Log("XR mode -> %d", int(g_settings.mode));
  }
  if (f10 && !f10Was) g_recenterRequested = true;
  f9Was = f9;
  f10Was = f10;

  if (g_settings.mode == Mode::Off) {
    if (g_instance) {
      if (g_frameBegun) EndFrame();
      DestroyAll();
      g_status = "off";
    }
    return;
  }

  if (!g_instance) {
    if (NowMs() < g_nextInitAttempt) return;
    if (!Init()) {
      DestroyAll();
      g_nextInitAttempt = NowMs() + kInitRetryMs;
      return;
    }
  }

  PollEvents();
  if (!g_session || !g_running) return;
  if (g_frameBegun) EndFrame();
  BeginFrame();
}

bool StereoFrameActive() {
  std::lock_guard lock(g_mutex);
  return g_running && g_frameBegun && (g_frameState.shouldRender || g_settings.bench) && g_viewsValid && g_haveRef &&
         g_settings.mode == Mode::Stereo;
}

Pose EyePose(int eye) {
  std::lock_guard lock(g_mutex);
  return g_eyeRel[eye & 1];
}

void EyeTanFov(int eye, float& tanHalfX, float& tanHalfY) {
  std::lock_guard lock(g_mutex);
  const XrFovf& f = g_views[eye & 1].fov;
  tanHalfX = std::max(std::tan(std::fabs(f.angleLeft)), std::tan(std::fabs(f.angleRight)));
  tanHalfY = std::max(std::tan(std::fabs(f.angleUp)), std::tan(std::fabs(f.angleDown)));
}

// ---- Eye image dump (F11): writes what each eye receives to %LOCALAPPDATA%\gdvr\eyeN.bmp ----

void DumpBackBuffer(int eye) {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g_gameSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
  D3D11_TEXTURE2D_DESC d;
  bb->GetDesc(&d);
  D3D11_TEXTURE2D_DESC sd = d;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sd.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  if (SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &staging))) {
    g_context->CopyResource(staging, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
      std::vector<uint8_t> px(size_t(d.Width) * d.Height * 3);
      for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* out = px.data() + size_t(d.Height - 1 - y) * d.Width * 3;  // BMP rows go bottom-up
        for (UINT x = 0; x < d.Width; ++x) {
          const uint8_t* p = row + x * 4;
          out[x * 3 + 0] = rgba ? p[2] : p[0];
          out[x * 3 + 1] = p[1];
          out[x * 3 + 2] = rgba ? p[0] : p[2];
        }
      }
      g_context->Unmap(staging, 0);
      wchar_t base[MAX_PATH];
      GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
      const std::wstring path = std::wstring(base) + L"\\gdvr\\eye" + std::to_wstring(eye) + L".bmp";
      FILE* f = nullptr;
      if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        const uint32_t dataSize = uint32_t(px.size()), fileSize = 54 + dataSize;
        uint8_t h[54] = {'B', 'M'};
        std::memcpy(h + 2, &fileSize, 4);
        const uint32_t off = 54, hs = 40, planesBpp = 1 | (24 << 16);
        std::memcpy(h + 10, &off, 4);
        std::memcpy(h + 14, &hs, 4);
        std::memcpy(h + 18, &d.Width, 4);
        std::memcpy(h + 22, &d.Height, 4);
        std::memcpy(h + 26, &planesBpp, 4);
        std::memcpy(h + 34, &dataSize, 4);
        fwrite(h, 1, 54, f);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
        Log("dumped eye %d image (%ux%u)", eye, d.Width, d.Height);
      }
    }
    staging->Release();
  }
  bb->Release();
}

void ApplyMask(const mask::Params& p) {
  std::lock_guard lock(g_mutex);
  static bool warned = false;
  if (!mask::Apply(g_gameSwapChain, p) && !warned) {
    warned = true;
    Log("mask: not drawn (no usable scene depth buffer found yet)");
  }
}

void PrepareHud() {
  std::lock_guard lock(g_mutex);
  if (!g_frameBegun || !g_settings.hud || !g_gameSwapChain) return;
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g_gameSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
  if (g_bbRtvTex != bb) {
    if (g_bbRtv) g_bbRtv->Release();
    g_bbRtv = nullptr;
    g_device->CreateRenderTargetView(bb, nullptr, &g_bbRtv);
    g_bbRtvTex = bb;
  }
  if (g_bbRtv) {
    const float transparent[4] = {0, 0, 0, 0};
    g_context->ClearRenderTargetView(g_bbRtv, transparent);
    g_hudPrepared = true;
  }
  bb->Release();
}

void CaptureEye(int eye, float tanHalfX, float tanHalfY) {
  std::lock_guard lock(g_mutex);
  if (!g_frameBegun) return;
  eye &= 1;
  if (g_dumpEyes & (1 << eye)) {
    DumpBackBuffer(eye);
    g_dumpEyes &= ~(1 << eye);
  }
  g_eyeCaptured[eye] = CaptureBackBuffer(g_eyeChains[eye]);
  g_eyeTan[eye][0] = tanHalfX;
  g_eyeTan[eye][1] = tanHalfY;
}

double LastWaitMs() { return g_lastWaitMs; }

void RequestEyeDump() { g_dumpEyes = 3; }

void Shutdown() {
  std::lock_guard lock(g_mutex);
  if (g_session && g_frameBegun) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_frameState.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    xrEndFrame(g_session, &ei);
  }
  DestroyAll();
  if (g_bbRtv) g_bbRtv->Release();
  g_bbRtv = nullptr;
  g_bbRtvTex = nullptr;
  if (g_context) g_context->Release();
  if (g_device) g_device->Release();
  g_context = nullptr;
  g_device = nullptr;
  g_gameSwapChain = nullptr;
}

void DrawDebugUI() {
  std::lock_guard lock(g_mutex);
  if (!ImGui::CollapsingHeader("VR (F9 mode, F10 recentre)", ImGuiTreeNodeFlags_DefaultOpen)) return;
  static const char* kModes[] = {"Off", "Screen", "Stereo"};
  ImGui::Text("Mode: %s   Status: %s", kModes[int(g_settings.mode)], g_status.c_str());
  ImGui::Text("Session state %d  running=%d  shouldRender=%d  frames submitted=%llu", int(g_state), int(g_running),
              int(g_frameState.shouldRender), (unsigned long long)g_framesSubmitted);
  ImGui::Text("Eyes captured: %d %d   tan(half fov) L %.3f/%.3f  R %.3f/%.3f", int(g_eyeCaptured[0]),
              int(g_eyeCaptured[1]), g_eyeTan[0][0], g_eyeTan[0][1], g_eyeTan[1][0], g_eyeTan[1][1]);
  ImGui::Text("Eye L rel pos %.3f %.3f %.3f", g_eyeRel[0].position.x, g_eyeRel[0].position.y, g_eyeRel[0].position.z);
  ImGui::Text("Scale %.1f  FOV %.0f  level=%d  frustum=%d", g_settings.worldScale, g_settings.fovDeg,
              int(g_settings.levelHorizon), int(g_settings.recomputeFrustum));
}

}  // namespace gdvr::xr
