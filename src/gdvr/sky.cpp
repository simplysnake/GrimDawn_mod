#include "sky.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include "d3dtrace.h"
#include "devotion.h"
#include "log.h"
#include "paths.h"
#include "pointer.h"
#include "xr.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

namespace gdvr::sky {

namespace {

template <typename T>
void SafeRelease(T*& p);  // below

ID3D11Texture2D* g_constTex = nullptr;
ID3D11ShaderResourceView* g_constSrv = nullptr;
ID3D11Texture2D* g_backTex = nullptr;
ID3D11ShaderResourceView* g_backSrv = nullptr;
bool g_loaded = false;
std::atomic<bool> g_reload{false};
std::atomic<int> g_captureCountdown{-1};  // frames until the capture (-1 = none)
std::string g_status = "not loaded";
std::wstring g_backdropPath;           // the backdrop file in use (watched for changes)
FILETIME g_backdropTime = {};
ULONGLONG g_lastWatch = 0;

FILETIME WriteTime(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return {};
  return a.ftLastWriteTime;
}

template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::wstring SkyDir() {
#ifdef GDVR_DEV_LOG_DIR
  std::wstring dir = GDVR_DEV_LOG_DIR;  // <repo>/logs -> <repo>/sky
  dir = dir.substr(0, dir.find_last_of(L"/\\")) + L"/sky";
#else
  wchar_t base[MAX_PATH] = L".";
  GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  std::wstring dir = std::wstring(base) + L"\\gdvr\\sky";
#endif
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

IWICImagingFactory* Wic() {
  static IWICImagingFactory* factory = nullptr;
  if (!factory) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // fine if the thread already has COM in another mode
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  }
  return factory;
}

// Image file -> RGBA8 pixels.
bool LoadImageFile(const std::wstring& path, std::vector<uint8_t>& rgba, UINT& w, UINT& h) {
  IWICImagingFactory* f = Wic();
  if (!f || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  bool ok = SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(conv->GetSize(&w, &h));
  if (ok) {
    rgba.resize(size_t(w) * h * 4);
    ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(rgba.size()), rgba.data()));
  }
  SafeRelease(conv);
  SafeRelease(frame);
  SafeRelease(dec);
  return ok;
}

// RGBA8 pixels -> PNG.
bool SavePng(const std::wstring& path, const uint8_t* rgba, UINT w, UINT h, UINT pitch) {
  IWICImagingFactory* f = Wic();
  if (!f) return false;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
  bool ok = SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) &&
            SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
            SUCCEEDED(frame->WritePixels(h, pitch, pitch * h, const_cast<uint8_t*>(rgba))) && SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(enc->Commit());
  SafeRelease(frame);
  SafeRelease(enc);
  SafeRelease(stream);
  return ok;
}

// A mipmapped texture (sampled at any distance without shimmering) from RGBA8 pixels.
bool MakeTexture(ID3D11Device* device, const std::vector<uint8_t>& rgba, UINT w, UINT h, ID3D11Texture2D*& tex,
                 ID3D11ShaderResourceView*& srv) {
  SafeRelease(srv);
  SafeRelease(tex);
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 0;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  if (FAILED(device->CreateTexture2D(&d, nullptr, &tex)) || FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
    SafeRelease(tex);
    return false;
  }
  ID3D11DeviceContext* ctx = nullptr;
  device->GetImmediateContext(&ctx);
  ctx->UpdateSubresource(tex, 0, nullptr, rgba.data(), w * 4, 0);
  ctx->GenerateMips(srv);
  ctx->Release();
  return true;
}

void LoadAll(ID3D11Device* device) {
  const std::wstring dir = SkyDir();
  std::vector<uint8_t> px;
  UINT w = 0, h = 0;
  std::string status;
  if (LoadImageFile(dir + L"/constellations.png", px, w, h) && MakeTexture(device, px, w, h, g_constTex, g_constSrv))
    status += "constellations " + std::to_string(w) + "x" + std::to_string(h);
  else
    status += "no constellations captured";
  // The backdrop: the user's own sky/backdrop.*, otherwise the first image in the mod's skybox folder.
  std::vector<std::wstring> candidates = {dir + L"/backdrop.png", dir + L"/backdrop.jpg", dir + L"/backdrop.jpeg"};
  const std::wstring skyboxDir = paths::InstallDir() + L"/skybox";
  for (const wchar_t* pattern : {L"/*.png", L"/*.jpg", L"/*.jpeg"}) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((skyboxDir + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) continue;
    do candidates.push_back(skyboxDir + L"/" + fd.cFileName);
    while (FindNextFileW(find, &fd));
    FindClose(find);
  }
  bool back = false;
  for (const std::wstring& path : candidates)
    if (LoadImageFile(path, px, w, h) && MakeTexture(device, px, w, h, g_backTex, g_backSrv)) {
      status += ", backdrop " + std::to_string(w) + "x" + std::to_string(h);
      Log("sky: backdrop %ls", path.c_str());
      g_backdropPath = path;
      g_backdropTime = WriteTime(path);
      back = true;
      break;
    }
  if (!back) status += ", no backdrop";
  g_status = status;
  Log("sky: %s (folder %ls)", status.c_str(), dir.c_str());
}

// The UI layer (back buffer) -> constellations.png, and the live texture.
void Capture(IDXGISwapChain* swapChain) {
  ID3D11Texture2D* bb = nullptr;
  ID3D11Device* device = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
  bb->GetDevice(&device);
  D3D11_TEXTURE2D_DESC d;
  bb->GetDesc(&d);
  D3D11_TEXTURE2D_DESC sd = d;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sd.MiscFlags = 0;
  sd.MipLevels = 1;
  ID3D11Texture2D* staging = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  device->GetImmediateContext(&ctx);
  if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging))) {
    ctx->CopyResource(staging, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
      std::vector<uint8_t> px(size_t(d.Width) * d.Height * 4);
      for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* out = px.data() + size_t(y) * d.Width * 4;
        for (UINT x = 0; x < d.Width; ++x) {
          const uint8_t* p = row + x * 4;
          out[x * 4 + 0] = bgra ? p[2] : p[0];
          out[x * 4 + 1] = p[1];
          out[x * 4 + 2] = bgra ? p[0] : p[2];
          out[x * 4 + 3] = 255;
        }
      }
      ctx->Unmap(staging, 0);
      const std::wstring path = SkyDir() + L"/constellations.png";
      const bool saved = SavePng(path, px.data(), d.Width, d.Height, d.Width * 4);
      MakeTexture(device, px, d.Width, d.Height, g_constTex, g_constSrv);
      g_status = std::string("constellations captured ") + std::to_string(d.Width) + "x" + std::to_string(d.Height) +
                 (saved ? "" : " (saving failed)");
      Log("sky: captured the Devotion screen %ux%u -> %ls %s", d.Width, d.Height, path.c_str(), saved ? "" : "(save FAILED)");
    }
    staging->Release();
  }
  ctx->Release();
  device->Release();
  bb->Release();
}

}  // namespace

Layers Get(IDXGISwapChain* swapChain) {
  // Auto-reload: a saved-over backdrop image is picked up within a couple of seconds.
  if (g_loaded && !g_backdropPath.empty() && GetTickCount64() - g_lastWatch > 2000) {
    g_lastWatch = GetTickCount64();
    const FILETIME t = WriteTime(g_backdropPath);
    if ((t.dwLowDateTime || t.dwHighDateTime) && CompareFileTime(&t, &g_backdropTime) != 0) {
      Log("sky: backdrop changed on disk, reloading");
      g_reload = true;
    }
  }
  if ((!g_loaded || g_reload.exchange(false)) && swapChain) {
    g_loaded = true;
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) {
      LoadAll(device);
      device->Release();
    }
  }
  if (xr::GetSettings().skySource == 0) {
    devotion::Start();
    devotion::SetLook(xr::GetSettings().skyIncomplete, xr::GetSettings().skyGlow);
    return {devotion::MapSrv(swapChain), g_backSrv, true};
  }
  return {g_constSrv, g_backSrv, false};
}

void RequestCapture() {
  d3dtrace::ArmUiTrace();  // also record what the Devotion screen draws (to filter out its frame later)
  pointer::ParkCursor(700.0);  // no hover text in the capture
  g_captureCountdown = 30;     // about half a second: the cursor is gone and the tooltip has faded
}

void RequestReload() { g_reload = true; }

namespace {

// "WxH fN, WxH fN" -> list
std::vector<std::string> SkipList() {
  std::vector<std::string> out;
  std::string all = xr::GetSettings().skySkip, item;
  for (size_t i = 0; i <= all.size(); ++i) {
    if (i == all.size() || all[i] == ',') {
      while (!item.empty() && item.front() == ' ') item.erase(item.begin());
      while (!item.empty() && item.back() == ' ') item.pop_back();
      if (!item.empty()) out.push_back(item);
      item.clear();
    } else {
      item += all[i];
    }
  }
  return out;
}

}  // namespace

void OnHudFrame(IDXGISwapChain* swapChain) {
  int n = g_captureCountdown.load();
  if (n < 0) return;
  if (n > 0) {
    // The UI drawn next is the one captured: leave the listed textures (frame, font...) out of it.
    if (n == 1) d3dtrace::SetUiSkip(SkipList(), true);
    g_captureCountdown = n - 1;
    return;
  }
  g_captureCountdown = -1;
  Capture(swapChain);
  if (d3dtrace::UiSkipped()) Log("sky: %d UI draws left out of the capture", d3dtrace::UiSkipped());
  d3dtrace::SetUiSkip({}, false);
}

void DrawDebugUI() {
  ImGui::TextDisabled("Sky: %s", g_status.c_str());
  ImGui::TextDisabled("Devotion map: %s", devotion::Status().c_str());
  if (xr::GetSettings().skySource == 1) {
    if (ImGui::Button("Capture constellations (open the Devotion screen first)")) RequestCapture();
    ImGui::SameLine();
  }
  if (ImGui::Button("Reload sky images")) RequestReload();
}

void Shutdown() {
  devotion::Shutdown();
  SafeRelease(g_constSrv);
  SafeRelease(g_constTex);
  SafeRelease(g_backSrv);
  SafeRelease(g_backTex);
  g_loaded = false;
}

}  // namespace gdvr::sky
