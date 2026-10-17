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
#include "overlay.h"
#include "sky.h"
#include "perf.h"
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
Pose g_handRel[2];        // controller aim poses relative to g_ref
bool g_handValid[2] = {};
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

// HUD look for this frame: the dark backing behind HUD elements follows scene brightness.
float g_hudLum = -1.0f;    // smoothed scene brightness
float g_hudDayAmount = 0;  // 0 = night look, 1 = daylight look
std::atomic<bool> g_hudMenuOpen{false};  // a game menu is open (from HUD coverage); read without g_mutex

mask::HudLook CurrentHudLook() {
  const float lum = mask::SceneBrightness();
  if (lum >= 0) g_hudLum = g_hudLum < 0 ? lum : g_hudLum + (lum - g_hudLum) * 0.04f;  // ~0.5 s at 50 fps
  g_hudDayAmount = 0;
  if (g_settings.hudForceDaylight) {
    g_hudDayAmount = 1;
  } else if (g_settings.hudDayAdapt && g_hudLum >= 0) {
    const float span = std::max(0.001f, g_settings.hudDayTo - g_settings.hudDayFrom);
    const float t = std::clamp((g_hudLum - g_settings.hudDayFrom) / span, 0.0f, 1.0f);
    g_hudDayAmount = t * t * (3 - 2 * t);  // smoothstep
  }
  static ULONGLONG lastLog = 0;
  if (GetTickCount64() - lastLog > 5000) {
    lastLog = GetTickCount64();
    Log("hud: scene brightness %.3f -> daylight %.2f, coverage %.3f (menu %d)", g_hudLum, g_hudDayAmount,
        mask::HudCoverage(), int(g_hudMenuOpen));
  }
  auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
  mask::HudLook look;
  look.brightness = g_settings.hudBrightness;
  look.opacity = g_settings.hudOpacity;
  look.halo = mix(g_settings.hudHalo, g_settings.hudHaloDay, g_hudDayAmount);
  look.haloRadius = mix(g_settings.hudHaloRadius, g_settings.hudHaloRadiusDay, g_hudDayAmount);
  look.alphaBoost = g_settings.hudAlphaBoost;
  // A menu (inventory, options, map...) covers much of the HUD image: put the block back while it's open.
  const float coverage = mask::HudCoverage();
  if (coverage >= 0) {
    if (!g_hudMenuOpen && coverage > g_settings.hudMenuCoverage) g_hudMenuOpen = true;
    if (g_hudMenuOpen && coverage < g_settings.hudMenuCoverage * 0.85f) g_hudMenuOpen = false;  // hysteresis
  }
  look.sideShift = g_hudMenuOpen ? 0.0f : g_settings.hudSideShift;
  look.sideWidth = g_settings.hudSideWidth;
  look.sideHeight = g_settings.hudSideHeight;
  look.sideOutline = g_settings.hudSideOutline;
  return look;
}

// Writes every persistent setting to control.ini [vr]. Caller holds g_mutex.
bool SaveSettingsLocked() {
  wchar_t base[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return false;
  const std::wstring path = std::wstring(base) + L"\\gdvr\\control.ini";
  const Settings& s = g_settings;
  bool ok = true;
  auto put = [&](const wchar_t* key, double v) {
    wchar_t buf[64];
    swprintf_s(buf, L"%g", v);
    ok &= WritePrivateProfileStringW(L"vr", key, buf, path.c_str()) != 0;
  };
  put(L"mode", int(s.mode));
  put(L"world_scale", s.worldScale);
  put(L"fov_deg", s.fovDeg);
  put(L"level_horizon", s.levelHorizon);
  put(L"recompute_frustum", s.recomputeFrustum);
  put(L"far_plane", s.farPlane);
  put(L"camera_pull", s.cameraPull);
  put(L"sky", s.sky);
  put(L"sky_source", s.skySource);
  put(L"sky_dome_horizon", s.skyDomeHorizonDeg);
  put(L"sky_incomplete", s.skyIncomplete);
  put(L"sky_glow", s.skyGlow);
  put(L"credit", s.credit);
  put(L"sky_auto_open", s.skyAutoOpen);
  {
    wchar_t wide[256];
    MultiByteToWideChar(CP_UTF8, 0, s.skySkip, -1, wide, 256);
    ok &= WritePrivateProfileStringW(L"vr", L"sky_skip_textures", wide, path.c_str()) != 0;
  }
  put(L"sky_band_width", s.skyBandWidthDeg);
  put(L"sky_band_low", s.skyBandLowDeg);
  put(L"sky_band_high", s.skyBandHighDeg);
  put(L"sky_band_yaw", s.skyBandYawDeg);
  put(L"sky_crop_left", s.skyCrop[0]);
  put(L"sky_crop_top", s.skyCrop[1]);
  put(L"sky_crop_right", s.skyCrop[2]);
  put(L"sky_crop_bottom", s.skyCrop[3]);
  put(L"sky_key", s.skyKey);
  put(L"sky_soft", s.skySoft);
  put(L"sky_brightness", s.skyBrightness);
  put(L"sky_opacity", s.skyOpacity);
  put(L"sky_backdrop_brightness", s.skyBackdropBrightness);
  put(L"screen_distance", s.screenDistance);
  put(L"screen_width", s.screenWidth);
  put(L"mask", s.maskEnabled);
  put(L"mask_inner", s.maskInner);
  put(L"mask_outer", s.maskOuter);
  put(L"mask_depth_start", s.maskDepthStart);
  put(L"mask_strength", s.maskStrength);
  put(L"mask_depth_pick", s.maskDepthPick);
  put(L"hud", s.hud);
  put(L"hud_distance", s.hudDistance);
  put(L"hud_width", s.hudWidth);
  put(L"hud_down", s.hudDown);
  put(L"hud_tilt", s.hudTiltDeg);
  put(L"hud_side_shift", s.hudSideShift);
  put(L"hud_side_width", s.hudSideWidth);
  put(L"hud_side_height", s.hudSideHeight);
  put(L"hud_menu_coverage", s.hudMenuCoverage);
  put(L"labels_in_world", s.labelsInWorld);
  put(L"damage_numbers_3d", s.damageNumbers3d);
  put(L"item_labels_3d", s.itemLabels3d);
  put(L"pointer", s.pointer);
  put(L"pointer_hand", s.pointerHand);
  put(L"pure_controller", s.pureController);
  put(L"hud_brightness", s.hudBrightness);
  put(L"hud_opacity", s.hudOpacity);
  put(L"hud_alpha_boost", s.hudAlphaBoost);
  put(L"hud_day_adapt", s.hudDayAdapt);
  put(L"hud_halo", s.hudHalo);
  put(L"hud_halo_radius", s.hudHaloRadius);
  put(L"hud_halo_day", s.hudHaloDay);
  put(L"hud_halo_radius_day", s.hudHaloRadiusDay);
  put(L"hud_day_from", s.hudDayFrom);
  put(L"hud_day_to", s.hudDayTo);
  put(L"keep_running_unfocused", s.keepRunningUnfocused);
  Log(ok ? "settings saved to control.ini" : "saving settings to control.ini failed");
  return ok;
}

// "Grim Dawn VR - created by artalva.com" on the HUD panel for the first seconds in VR (fades out).
void DrawCredit() {
  static ULONGLONG start = 0;
  if (!g_settings.credit) return;
  if (!start) start = GetTickCount64();
  const double t = double(GetTickCount64() - start) / 1000.0;
  constexpr double kShow = 8.0, kFade = 2.0;
  if (t > kShow) return;
  const float alpha = float(t < kShow - kFade ? 1.0 : (kShow - t) / kFade);
  const unsigned a = unsigned(alpha * 255.0f) << 24;
  const float w = float(g_hudChain.width), h = float(g_hudChain.height);
  overlay::WorldText lines[2] = {
      {w * 0.5f, h * 0.30f, h * 0.06f, 0x00FFFFFFu | a, "Grim Dawn VR", false},
      {w * 0.5f, h * 0.37f, h * 0.035f, 0x00B0D8FFu | a, "created by artalva.com", false},
  };
  overlay::DrawWorldText(lines, 2);
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
        sky::OnHudFrame(g_gameSwapChain);  // Devotion screen capture (UI layer, before the HUD pass)
        DrawCredit();
        mask::FixHudAlpha(g_gameSwapChain, CurrentHudLook());
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
    } else if (g_settings.mode != Mode::Off && (sky::OnHudFrame(g_gameSwapChain), CaptureBackBuffer(g_screenChain))) {
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
  for (int hand = 0; hand < 2; ++hand) {
    XrPosef aim;
    g_handValid[hand] = g_haveRef && input::LocateAim(hand, g_localSpace, g_frameState.predictedDisplayTime, aim);
    if (g_handValid[hand]) g_handRel[hand] = Relative(g_ref, ToPose(aim));
  }

  // Controllers -> virtual gamepad (read by the game through XInputGetState).
  if (input::Sync(g_session, g_state == XR_SESSION_STATE_FOCUSED)) g_recenterRequested = true;
}

}  // namespace

Settings& GetSettings() { return g_settings; }
void RequestRecenter() { g_recenterRequested = true; }

bool SaveSettings() {
  std::lock_guard lock(g_mutex);
  return SaveSettingsLocked();
}

void OnPresent(IDXGISwapChain* swapChain) {
  d3dtrace::EndUi();
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

bool HudPanelPoint(Vec3 pointRel, float& u, float& v) {
  std::lock_guard lock(g_mutex);
  // Panel pose relative to the recentred origin (EndFrame adds the recentre yaw/position on top).
  const float tilt = -g_settings.hudTiltDeg * 3.14159265f / 180.0f;
  const Quat pitch{std::sin(tilt * 0.5f), 0, 0, std::cos(tilt * 0.5f)};
  const Vec3 centre{0, -g_settings.hudDown, -g_settings.hudDistance};
  const Vec3 axisX = Rotate(pitch, {1, 0, 0}), axisY = Rotate(pitch, {0, 1, 0}), normal = Rotate(pitch, {0, 0, 1});
  const float width = g_settings.hudWidth;
  const float height = width * float(g_hudChain.height) / float(std::max(1u, g_hudChain.width));
  const Vec3 eye = (g_eyeRel[0].position + g_eyeRel[1].position) * 0.5f;
  const Vec3 dir = pointRel - eye;
  const float denom = Dot(normal, dir);
  if (std::fabs(denom) < 1e-6f) return false;
  const float t = Dot(normal, centre - eye) / denom;
  if (t <= 0) return false;
  const Vec3 local = eye + dir * t - centre;
  u = Dot(local, axisX) / width + 0.5f;
  v = 0.5f - Dot(local, axisY) / height;
  return true;
}

bool HandAim(int hand, Pose& pose) {
  std::lock_guard lock(g_mutex);
  if (!g_handValid[hand & 1]) return false;
  pose = g_handRel[hand & 1];
  return true;
}

bool HudPanelRay(Vec3 originRel, Vec3 dirRel, float& u, float& v, float& distance) {
  std::lock_guard lock(g_mutex);
  if (!g_settings.hud) return false;
  const float tilt = -g_settings.hudTiltDeg * 3.14159265f / 180.0f;
  const Quat pitch{std::sin(tilt * 0.5f), 0, 0, std::cos(tilt * 0.5f)};
  const Vec3 centre{0, -g_settings.hudDown, -g_settings.hudDistance};
  const Vec3 axisX = Rotate(pitch, {1, 0, 0}), axisY = Rotate(pitch, {0, 1, 0}), normal = Rotate(pitch, {0, 0, 1});
  const float width = g_settings.hudWidth;
  const float height = width * float(g_hudChain.height) / float(std::max(1u, g_hudChain.width));
  const float denom = Dot(normal, dirRel);
  if (std::fabs(denom) < 1e-6f) return false;
  const float t = Dot(normal, centre - originRel) / denom;
  if (t <= 0) return false;
  const Vec3 local = originRel + dirRel * t - centre;
  u = Dot(local, axisX) / width + 0.5f;
  v = 0.5f - Dot(local, axisY) / height;
  distance = t;
  return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

bool HudSolidAt(float u, float v, bool sticky) {
  std::lock_guard lock(g_mutex);
  // A menu is open: the whole panel catches the pointer (no falling through gaps in a menu).
  if (g_hudMenuOpen) return true;
  // The minimap & quest block never catches the pointer.
  const float left = 1.0f - g_settings.hudSideWidth - g_settings.hudSideShift;
  if (u >= left && u <= left + g_settings.hudSideWidth && v <= g_settings.hudSideHeight) return false;
  // Already on the HUD: stay on it over faint parts too, so the pointer doesn't flicker through.
  const float a = mask::HudAlphaAt(u, v);
  return a < 0 || a > (sticky ? 0.08f : 0.3f);  // not measured yet: treat the panel as solid
}

bool HudMenuOpen() {
  return g_hudMenuOpen.load();  // no lock: also called from code that runs while g_mutex is held
}

IDXGISwapChain* GameSwapChain() { return g_gameSwapChain; }

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
  if (eye == 0 && g_settings.hudDayAdapt) mask::MeasureBrightness(g_gameSwapChain);
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
  static const char* kModes[] = {"Off", "Screen", "Stereo"};
  ImGui::Text("%s  |  %s  |  %.0f fps (%.1f ms)", kModes[int(g_settings.mode)], g_status.c_str(), perf::LastFps(),
              perf::LastFrameMs());
  ImGui::TextDisabled("F9 mode  F10 recentre (or both sticks)  F7 fade mask  F11 eye images");
  ImGui::Separator();

  if (ImGui::CollapsingHeader("HUD placement", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SliderFloat("Distance (m)", &g_settings.hudDistance, 0.25f, 2.5f, "%.2f");
    float height = -g_settings.hudDown;  // shown as up = positive
    if (ImGui::SliderFloat("Height (m, + up)", &height, -0.8f, 0.5f, "%.2f")) g_settings.hudDown = -height;
    ImGui::SliderFloat("Size (m wide)", &g_settings.hudWidth, 0.3f, 2.5f, "%.2f");
    ImGui::SliderFloat("Tilt (deg)", &g_settings.hudTiltDeg, -10.0f, 60.0f, "%.0f");
    ImGui::SeparatorText("Minimap & quests (top right)");
    ImGui::SliderFloat("Move left", &g_settings.hudSideShift, 0.0f, 0.78f, "%.2f");
    ImGui::SliderFloat("Block width", &g_settings.hudSideWidth, 0.05f, 0.5f, "%.2f");
    ImGui::SliderFloat("Block height", &g_settings.hudSideHeight, 0.05f, 1.0f, "%.2f");
    ImGui::Checkbox("Show block outline", &g_settings.hudSideOutline);
    ImGui::Checkbox("3D damage numbers", &g_settings.damageNumbers3d);
    ImGui::SameLine();
    ImGui::Checkbox("3D item names", &g_settings.itemLabels3d);
    ImGui::Checkbox("Labels over their monsters/items (health bars, names)", &g_settings.labelsInWorld);
    ImGui::Text("HUD coverage %.3f  ->  %s", mask::HudCoverage(),
                g_hudMenuOpen ? "menu open (block in place)" : "no menu");
    ImGui::SliderFloat("Menu open above", &g_settings.hudMenuCoverage, 0.05f, 0.8f, "%.2f");
    if (ImGui::Button("Recentre view")) g_recenterRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset placement")) {
      const Settings d;
      g_settings.hudDistance = d.hudDistance;
      g_settings.hudDown = d.hudDown;
      g_settings.hudWidth = d.hudWidth;
      g_settings.hudTiltDeg = d.hudTiltDeg;
      g_settings.hudSideShift = d.hudSideShift;
      g_settings.hudSideWidth = d.hudSideWidth;
      g_settings.hudSideHeight = d.hudSideHeight;
      g_settings.hudMenuCoverage = d.hudMenuCoverage;
    }
  }

  if (ImGui::CollapsingHeader("HUD look", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Scene brightness %.3f  ->  daylight %.2f", g_hudLum, g_hudDayAmount);
    ImGui::SliderFloat("Brightness", &g_settings.hudBrightness, 0.1f, 1.5f);
    ImGui::SliderFloat("Opacity", &g_settings.hudOpacity, 0.1f, 1.0f);
    ImGui::SliderFloat("Solid dark parts", &g_settings.hudAlphaBoost, 1.0f, 8.0f);
    ImGui::Checkbox("Stronger backing in daylight", &g_settings.hudDayAdapt);
    ImGui::SameLine();
    ImGui::Checkbox("Preview daylight", &g_settings.hudForceDaylight);
    ImGui::SliderFloat("Night halo strength", &g_settings.hudHalo, 0.0f, 1.0f);
    ImGui::SliderFloat("Night halo size (px)", &g_settings.hudHaloRadius, 1.0f, 32.0f);
    ImGui::SliderFloat("Day halo strength", &g_settings.hudHaloDay, 0.0f, 1.0f);
    ImGui::SliderFloat("Day halo size (px)", &g_settings.hudHaloRadiusDay, 1.0f, 32.0f);
    ImGui::SliderFloat("Night below brightness", &g_settings.hudDayFrom, 0.0f, 0.6f);
    ImGui::SliderFloat("Full day above brightness", &g_settings.hudDayTo, 0.05f, 0.8f);
    if (ImGui::Button("Reset HUD look")) {
      const Settings d;
      g_settings.hudBrightness = d.hudBrightness;
      g_settings.hudOpacity = d.hudOpacity;
      g_settings.hudAlphaBoost = d.hudAlphaBoost;
      g_settings.hudDayAdapt = d.hudDayAdapt;
      g_settings.hudHalo = d.hudHalo;
      g_settings.hudHaloRadius = d.hudHaloRadius;
      g_settings.hudHaloDay = d.hudHaloDay;
      g_settings.hudHaloRadiusDay = d.hudHaloRadiusDay;
      g_settings.hudDayFrom = d.hudDayFrom;
      g_settings.hudDayTo = d.hudDayTo;
    }
  }

  if (ImGui::CollapsingHeader("Pointer", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Checkbox("Pure controller (gamepad mode, no laser)", &g_settings.pureController);
    ImGui::Checkbox("Controller laser", &g_settings.pointer);
    ImGui::SameLine();
    ImGui::RadioButton("Left hand", &g_settings.pointerHand, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Right hand", &g_settings.pointerHand, 1);
  }

  if (ImGui::CollapsingHeader("Sky (constellations)")) {
    ImGui::Checkbox("Sky beyond the play area (needs the fade mask)", &g_settings.sky);
    ImGui::SameLine();
    ImGui::Checkbox("Start-up credit", &g_settings.credit);
    ImGui::Checkbox("Refresh constellations at start (opens the Devotion screen with D for a moment)", &g_settings.skyAutoOpen);
    ImGui::InputText("Leave out of capture", g_settings.skySkip, sizeof(g_settings.skySkip));
    ImGui::RadioButton("Devotion map from the game files (dome)", &g_settings.skySource, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Screen capture (band)", &g_settings.skySource, 1);
    if (g_settings.skySource == 0) {
      ImGui::SliderFloat("Dome rim elevation (deg)", &g_settings.skyDomeHorizonDeg, -45.0f, 30.0f, "%.0f");
      ImGui::SliderFloat("Not completed: brightness", &g_settings.skyIncomplete, 0.0f, 1.0f, "%.2f");
      ImGui::SliderFloat("Completed: glow", &g_settings.skyGlow, 0.0f, 1.5f, "%.2f");
    }
    sky::DrawDebugUI();
    ImGui::SliderFloat("Turn (deg)", &g_settings.skyBandYawDeg, -180.0f, 180.0f, "%.0f");
    if (g_settings.skySource == 1) {  // screen capture only
      ImGui::SliderFloat("Band width (deg)", &g_settings.skyBandWidthDeg, 40.0f, 360.0f, "%.0f");
      ImGui::SliderFloat("Band bottom (deg)", &g_settings.skyBandLowDeg, -60.0f, 60.0f, "%.0f");
      ImGui::SliderFloat("Band top (deg)", &g_settings.skyBandHighDeg, 0.0f, 90.0f, "%.0f");
      ImGui::SliderFloat4("Crop L T R B", g_settings.skyCrop, 0.0f, 1.0f, "%.2f");
      ImGui::SliderFloat("Background cut", &g_settings.skyKey, 0.0f, 0.6f, "%.2f");
      ImGui::SliderFloat("Cut softness", &g_settings.skySoft, 0.01f, 0.5f, "%.2f");
    }
    ImGui::SliderFloat("Constellation brightness", &g_settings.skyBrightness, 0.2f, 3.0f, "%.2f");
    ImGui::SliderFloat("Constellation opacity", &g_settings.skyOpacity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Backdrop brightness", &g_settings.skyBackdropBrightness, 0.0f, 2.0f, "%.2f");
  }

  if (ImGui::CollapsingHeader("World")) {
    ImGui::SliderFloat("World scale", &g_settings.worldScale, 4.0f, 40.0f, "%.1f");
    ImGui::SliderFloat("Camera closer (units)", &g_settings.cameraPull, 0.0f, 20.0f, "%.1f");
    ImGui::Checkbox("Fade mask", &g_settings.maskEnabled);
    ImGui::SliderFloat("Fade starts (units)", &g_settings.maskInner, 10.0f, 80.0f, "%.0f");
    ImGui::SliderFloat("Fully dark at (units)", &g_settings.maskOuter, 15.0f, 90.0f, "%.0f");
  }

  ImGui::Separator();
  static double savedAt = -10;
  if (ImGui::Button("Save settings")) {
    if (SaveSettingsLocked()) savedAt = ImGui::GetTime();
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset all to defaults")) {
    const Mode mode = g_settings.mode;
    g_settings = Settings{};
    g_settings.mode = mode;
  }
  if (ImGui::GetTime() - savedAt < 3) {
    ImGui::SameLine();
    ImGui::TextUnformatted("Saved to control.ini");
  }
}

void DrawDiagnostics() {
  std::lock_guard lock(g_mutex);
  ImGui::Text("Session state %d  running=%d  shouldRender=%d  frames submitted=%llu", int(g_state), int(g_running),
              int(g_frameState.shouldRender), (unsigned long long)g_framesSubmitted);
  ImGui::Text("Eyes captured %d %d   lens tan L %.3f/%.3f  R %.3f/%.3f", int(g_eyeCaptured[0]), int(g_eyeCaptured[1]),
              g_eyeTan[0][0], g_eyeTan[0][1], g_eyeTan[1][0], g_eyeTan[1][1]);
  ImGui::Text("Scale %.1f  FOV %.0f  far %.0f  level=%d", g_settings.worldScale, g_settings.fovDeg, g_settings.farPlane,
              int(g_settings.levelHorizon));
}

}  // namespace gdvr::xr
