#include "controllers.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wincodec.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "input.h"
#include "log.h"
#include "mask.h"
#include "vr_math.h"
#include "xr.h"

namespace gdvr::controllers {

namespace {

template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

struct Vertex {
  float pos[3], normal[3], uv[2];
};

// A model as read from disk (loader thread).
struct CpuModel {
  std::string name;   // render model name (folder)
  std::wstring dir;   // its folder
  std::vector<Vertex> verts;
  std::vector<uint8_t> rgba;  // diffuse texture (empty: plain grey)
  UINT texW = 0, texH = 0;
  // The model's "tip" component: where the runtime's aim pose sits in the model (model = SteamVR's raw pose).
  Quat tipRot;
  Vec3 tipPos;
  bool haveTip = false;
  std::string error;
};

struct HandModel {
  std::string profile;       // the interaction profile it was loaded for
  std::shared_ptr<CpuModel> cpu;
  ID3D11Buffer* vb = nullptr;
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  UINT vertexCount = 0;
  bool uploaded = false;
};

std::mutex g_loadMutex;  // guards g_loaded
std::shared_ptr<CpuModel> g_loaded[2];  // finished loads, waiting for the render thread
std::atomic<int> g_loading[2] = {};
HandModel g_hands[2];

ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11Texture2D* g_depthTex = nullptr;
ID3D11DepthStencilView* g_dsv = nullptr;
UINT g_depthW = 0, g_depthH = 0;
bool g_failed = false;

constexpr char kShader[] = R"(
cbuffer C : register(b0) {
  row_major float4x4 mvp;
  row_major float4x4 mv;
  float4 opts;  // x: textured
};
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VIn { float3 pos : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD; };
struct VOut { float4 pos : SV_Position; float3 n : NORMAL; float2 uv : TEXCOORD; };
VOut VS(VIn i) {
  VOut o;
  o.pos = mul(mvp, float4(i.pos, 1));
  o.n = mul((float3x3)mv, i.n);
  o.uv = i.uv;
  return o;
}
float4 PS(VOut i) : SV_Target {
  float3 base = opts.x > 0 ? tex.Sample(samp, i.uv).rgb : float3(0.35, 0.35, 0.37);
  float3 l = normalize(float3(0.3, 0.7, 0.6));  // from above, on the viewer's side
  float d = abs(dot(normalize(i.n), l));
  return float4(base * (0.35 + 0.75 * d), 1);
}
)";

// ---- Finding and reading SteamVR's render models -------------------------------------------------------

std::wstring Widen(const std::string& s) {
  std::wstring w(s.size(), L'\0');
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
  w.resize(std::max(0, n));
  return w;
}


std::string ReadText(const std::wstring& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool IsDir(const std::wstring& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// SteamVR's folder, from %LOCALAPPDATA%\openvr\openvrpaths.vrpath ("runtime": ["G:\\...\\SteamVR"]).
std::wstring SteamVrDir() {
  wchar_t base[MAX_PATH] = L"";
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return L"";
  const std::string text = ReadText(std::wstring(base) + L"\\openvr\\openvrpaths.vrpath");
  size_t at = text.find("\"runtime\"");
  if (at == std::string::npos || (at = text.find('"', text.find('[', at) + 1)) == std::string::npos) return L"";
  std::string path;
  for (size_t i = at + 1; i < text.size() && text[i] != '"'; ++i) {
    if (text[i] == '\\' && i + 1 < text.size()) ++i;  // JSON escape
    path += text[i];
  }
  return Widen(path);
}

// The render model folder for `name`: SteamVR's own models, then each driver's.
std::wstring FindModelDir(const std::wstring& steamvr, const std::string& name) {
  const std::wstring wname = Widen(name);
  std::wstring dir = steamvr + L"\\resources\\rendermodels\\" + wname;
  if (IsDir(dir)) return dir;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((steamvr + L"\\drivers\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return L"";
  std::wstring found;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
    dir = steamvr + L"\\drivers\\" + fd.cFileName + L"\\resources\\rendermodels\\" + wname;
    if (IsDir(dir)) found = dir;
  } while (found.empty() && FindNextFileW(h, &fd));
  FindClose(h);
  return found;
}

// Render model name for an interaction profile and hand.
std::string ModelName(const std::string& profile, int hand) {
  const char* side = hand ? "right" : "left";
  if (profile.find("valve/index_controller") != std::string::npos) return std::string("valve_controller_knu_1_0_") + side;
  if (profile.find("oculus/touch_controller") != std::string::npos ||
      profile.find("meta/touch") != std::string::npos)
    return std::string("oculus_quest2_controller_") + side;
  if (profile.find("htc/vive_controller") != std::string::npos) return "vr_controller_vive_1_5";
  return "generic_controller";
}

// Reads three numbers after `key` (a JSON array) starting at `from`.
bool ReadVec(const std::string& text, size_t from, size_t to, const char* key, float out[3]) {
  size_t at = text.find(key, from);
  if (at == std::string::npos || at > to || (at = text.find('[', at)) == std::string::npos) return false;
  const char* p = text.c_str() + at + 1;
  for (int k = 0; k < 3; ++k) {
    char* end = nullptr;
    out[k] = std::strtof(p, &end);
    if (end == p) return false;
    p = end;
    while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  }
  return true;
}

Quat AxisAngle(Vec3 axis, float deg) {
  const float h = deg * 3.14159265f / 360.0f;
  return {axis.x * std::sin(h), axis.y * std::sin(h), axis.z * std::sin(h), std::cos(h)};
}

// The "tip" component's place in the model (origin + rotate_xyz in degrees, X then Y then Z).
void ReadTip(const std::wstring& json, CpuModel& m) {
  const std::string text = ReadText(json);
  const size_t at = text.find("\"tip\"");
  if (at == std::string::npos) return;
  const size_t end = text.find('}', text.find("component_local", at));
  float o[3], r[3];
  if (!ReadVec(text, at, end, "\"origin\"", o) || !ReadVec(text, at, end, "\"rotate_xyz\"", r)) return;
  m.tipPos = {o[0], o[1], o[2]};
  m.tipRot = AxisAngle({1, 0, 0}, r[0]) * AxisAngle({0, 1, 0}, r[1]) * AxisAngle({0, 0, 1}, r[2]);
  m.haveTip = true;
}

bool LoadPng(const std::wstring& path, CpuModel& m) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* f = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  bool ok = SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(conv->GetSize(&m.texW, &m.texH));
  if (ok) {
    m.rgba.resize(size_t(m.texW) * m.texH * 4);
    ok = SUCCEEDED(conv->CopyPixels(nullptr, m.texW * 4, UINT(m.rgba.size()), m.rgba.data()));
  }
  if (!ok) m.rgba.clear();
  SafeRelease(conv);
  SafeRelease(frame);
  SafeRelease(dec);
  SafeRelease(f);
  return ok;
}

// OBJ: v / vt / vn and polygon faces (fanned into triangles), the texture from its .mtl's map_Kd.
bool LoadObj(const std::wstring& path, CpuModel& m) {
  std::ifstream f(path);
  if (!f) return false;
  std::vector<Vec3> pos, nrm;
  std::vector<std::pair<float, float>> uv;
  std::string line, mtllib;
  while (std::getline(f, line)) {
    const char* s = line.c_str();
    while (*s == ' ' || *s == '\t') ++s;
    if (s[0] == 'v' && (s[1] == ' ' || s[1] == '\t')) {
      Vec3 v;
      v.x = std::strtof(s + 2, const_cast<char**>(&s));
      v.y = std::strtof(s, const_cast<char**>(&s));
      v.z = std::strtof(s, const_cast<char**>(&s));
      pos.push_back(v);
    } else if (s[0] == 'v' && s[1] == 'n') {
      Vec3 v;
      v.x = std::strtof(s + 2, const_cast<char**>(&s));
      v.y = std::strtof(s, const_cast<char**>(&s));
      v.z = std::strtof(s, const_cast<char**>(&s));
      nrm.push_back(v);
    } else if (s[0] == 'v' && s[1] == 't') {
      float a = std::strtof(s + 2, const_cast<char**>(&s));
      float b = std::strtof(s, const_cast<char**>(&s));
      uv.push_back({a, b});
    } else if (s[0] == 'f' && (s[1] == ' ' || s[1] == '\t')) {
      std::vector<Vertex> poly;
      std::istringstream in(s + 2);
      std::string tok;
      while (in >> tok) {
        int idx[3] = {0, 0, 0};
        const char* t = tok.c_str();
        for (int k = 0; k < 3 && *t; ++k) {
          if (*t != '/') idx[k] = std::atoi(t);
          while (*t && *t != '/') ++t;
          if (*t == '/') ++t;
        }
        auto fix = [](int i, size_t n) { return i < 0 ? int(n) + i : i - 1; };  // 1-based, negative = from the end
        Vertex v{};
        const int pi = fix(idx[0], pos.size()), ti = fix(idx[1], uv.size()), ni = fix(idx[2], nrm.size());
        if (pi < 0 || pi >= int(pos.size())) continue;
        v.pos[0] = pos[pi].x, v.pos[1] = pos[pi].y, v.pos[2] = pos[pi].z;
        if (idx[1] && ti >= 0 && ti < int(uv.size())) v.uv[0] = uv[ti].first, v.uv[1] = 1.0f - uv[ti].second;
        if (idx[2] && ni >= 0 && ni < int(nrm.size())) v.normal[0] = nrm[ni].x, v.normal[1] = nrm[ni].y, v.normal[2] = nrm[ni].z;
        poly.push_back(v);
      }
      for (size_t k = 2; k < poly.size(); ++k) {
        m.verts.push_back(poly[0]);
        m.verts.push_back(poly[k - 1]);
        m.verts.push_back(poly[k]);
      }
    } else if (std::strncmp(s, "mtllib", 6) == 0) {
      mtllib = s + 6;
      mtllib.erase(0, mtllib.find_first_not_of(" \t"));
      while (!mtllib.empty() && (mtllib.back() == '\r' || mtllib.back() == ' ')) mtllib.pop_back();
    }
  }
  if (m.verts.empty()) return false;
  if (!mtllib.empty()) {
    std::istringstream mtl(ReadText(m.dir + L"\\" + Widen(mtllib)));
    while (std::getline(mtl, line)) {
      const size_t at = line.find("map_Kd");
      if (at == std::string::npos) continue;
      std::string file = line.substr(at + 6);
      file.erase(0, file.find_first_not_of(" \t"));
      while (!file.empty() && (file.back() == '\r' || file.back() == ' ')) file.pop_back();
      LoadPng(m.dir + L"\\" + Widen(file), m);  // a format WIC can't read (TGA): plain grey
      break;
    }
  }
  return true;
}

void LoadAsync(int hand, std::string profile) {
  ++g_loading[hand];
  std::thread([hand, profile] {
    auto m = std::make_shared<CpuModel>();
    m->name = ModelName(profile, hand);
    const std::wstring steamvr = SteamVrDir();
    m->dir = steamvr.empty() ? L"" : FindModelDir(steamvr, m->name);
    if (steamvr.empty()) {
      m->error = "SteamVR folder not found";
    } else if (m->dir.empty()) {
      m->error = "model folder not found";
    } else if (!LoadObj(m->dir + L"\\" + Widen(m->name) + L".obj", *m)) {
      m->error = "model file unreadable";
    } else {
      ReadTip(m->dir + L"\\" + Widen(m->name) + L".json", *m);
    }
    if (m->error.empty())
      Log("controllers: %s model %s: %zu triangles, texture %ux%u%s", hand ? "right" : "left", m->name.c_str(),
          m->verts.size() / 3, m->texW, m->texH, m->haveTip ? "" : ", no tip (aim pose = model origin)");
    else
      Log("controllers: %s model %s: %s", hand ? "right" : "left", m->name.c_str(), m->error.c_str());
    {
      std::lock_guard lock(g_loadMutex);
      g_loaded[hand] = m;
    }
    --g_loading[hand];
  }).detach();
}

// ---- Drawing ---------------------------------------------------------------------------------------------

void ReleaseHand(HandModel& h) {
  SafeRelease(h.vb);
  SafeRelease(h.srv);
  SafeRelease(h.tex);
  h.vertexCount = 0;
  h.uploaded = false;
}

bool EnsurePipeline(ID3D11Device* dev) {
  if (g_vs) return true;
  if (g_failed) return false;
  g_device = dev;
  ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_controllers", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_controllers", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &err));
  if (!ok && err) Log("controllers: shader: %s", static_cast<const char*>(err->GetBufferPointer()));
  const D3D11_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  ok = ok && SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
       SUCCEEDED(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps)) &&
       SUCCEEDED(dev->CreateInputLayout(layout, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &g_layout));
  SafeRelease(vs);
  SafeRelease(ps);
  SafeRelease(err);
  D3D11_BUFFER_DESC bd{144, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_ANISOTROPIC;
  sd.MaxAnisotropy = 4;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;  // the models' winding differs between left and right
  rd.DepthClipEnable = TRUE;
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_LESS;
  ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb)) && SUCCEEDED(dev->CreateSamplerState(&sd, &g_samp)) &&
       SUCCEEDED(dev->CreateRasterizerState(&rd, &g_raster)) && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_depthState));
  if (!ok) {
    Log("controllers: drawing unavailable");
    g_failed = true;
  }
  return ok;
}

bool EnsureDepth(UINT w, UINT h) {
  if (g_dsv && g_depthW == w && g_depthH == h) return true;
  SafeRelease(g_dsv);
  SafeRelease(g_depthTex);
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = d.ArraySize = 1;
  d.Format = DXGI_FORMAT_D32_FLOAT;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_depthTex)) ||
      FAILED(g_device->CreateDepthStencilView(g_depthTex, nullptr, &g_dsv)))
    return false;
  g_depthW = w;
  g_depthH = h;
  return true;
}

bool Upload(HandModel& h) {
  const CpuModel& m = *h.cpu;
  D3D11_BUFFER_DESC bd{UINT(m.verts.size() * sizeof(Vertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA init{m.verts.data(), 0, 0};
  if (FAILED(g_device->CreateBuffer(&bd, &init, &h.vb))) return false;
  h.vertexCount = UINT(m.verts.size());
  if (!m.rgba.empty()) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = m.texW;
    d.Height = m.texH;
    d.MipLevels = 0;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &h.tex)) &&
        SUCCEEDED(g_device->CreateShaderResourceView(h.tex, nullptr, &h.srv))) {
      ID3D11DeviceContext* ctx = nullptr;
      g_device->GetImmediateContext(&ctx);
      ctx->UpdateSubresource(h.tex, 0, nullptr, m.rgba.data(), m.texW * 4, 0);
      ctx->GenerateMips(h.srv);
      ctx->Release();
    } else {
      SafeRelease(h.tex);
    }
  }
  h.uploaded = true;
  return true;
}

// Keeps each hand's model in step with its controller (loads a new one when the profile changes).
void UpdateModels() {
  for (int hand = 0; hand < 2; ++hand) {
    HandModel& h = g_hands[hand];
    {
      std::lock_guard lock(g_loadMutex);
      if (g_loaded[hand]) {
        ReleaseHand(h);
        h.cpu = std::move(g_loaded[hand]);
        g_loaded[hand].reset();
      }
    }
    const std::string profile = input::HandProfile(hand);
    if (!profile.empty() && profile != h.profile && g_loading[hand] == 0) {
      h.profile = profile;
      LoadAsync(hand, profile);
    }
    if (h.cpu && h.cpu->error.empty() && !h.uploaded && g_device) Upload(h);
  }
}

void Mul(const float a[16], const float b[16], float out[16]) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

}  // namespace

void DrawEye(int eye, float tanX, float tanY) {
  const xr::Settings& s = xr::GetSettings();
  IDXGISwapChain* swapChain = xr::GameSwapChain();
  if (!s.controllerModels || g_failed || !swapChain) return;
  if (!g_device) {
    ID3D11Device* dev = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&dev)))) return;
    g_device = dev;  // the game's device outlives the mod's use of it
    dev->Release();
  }
  UpdateModels();
  Pose aim[2];
  bool any = false;
  for (int hand = 0; hand < 2; ++hand) {
    const HandModel& h = g_hands[hand];
    any |= h.uploaded && h.vertexCount && xr::HandAim(hand, aim[hand]);
  }
  if (!any) return;
  const Pose view = xr::EyePose(eye);
  mask::DrawOverBackBuffer(swapChain, [&](ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h) {
    if (!EnsurePipeline(g_device) || !EnsureDepth(w, h)) return;
    ID3D11Buffer* savedVb = nullptr;
    UINT savedStride = 0, savedOffset = 0;
    ctx->IAGetVertexBuffers(0, 1, &savedVb, &savedStride, &savedOffset);

    ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    mask::BindTargets(ctx, rtv, g_dsv);
    const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_depthState, 0);
    ctx->IASetInputLayout(g_layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetSamplers(0, 1, &g_samp);

    // Projection for this eye (OpenXR view axes: -Z forward), depth 0..1 between near and far.
    constexpr float n = 0.02f, f = 20.0f;
    const float proj[16] = {1 / tanX, 0, 0, 0, 0, 1 / tanY, 0, 0, 0, 0, -f / (f - n), -f * n / (f - n), 0, 0, -1, 0};
    const Quat toView = Conjugate(view.orientation);
    for (int hand = 0; hand < 2; ++hand) {
      const HandModel& hm = g_hands[hand];
      if (!hm.uploaded || !hm.vertexCount || !xr::HandAim(hand, aim[hand])) continue;
      // Model space is the controller's raw pose; the aim pose is the model's tip: raw = aim * tip^-1.
      const Quat tipInv = Conjugate(hm.cpu->tipRot);
      const Quat rawRot = aim[hand].orientation * tipInv;
      const Vec3 rawPos = aim[hand].position + Rotate(rawRot, hm.cpu->tipPos * -1.0f);
      const Quat q = toView * rawRot;
      const Vec3 t = Rotate(toView, rawPos - view.position);
      const Vec3 cx = Rotate(q, {1, 0, 0}), cy = Rotate(q, {0, 1, 0}), cz = Rotate(q, {0, 0, 1});
      const float mv[16] = {cx.x, cy.x, cz.x, t.x, cx.y, cy.y, cz.y, t.y, cx.z, cy.z, cz.z, t.z, 0, 0, 0, 1};
      struct {
        float mvp[16], mv[16], opts[4];
      } c{};
      Mul(proj, mv, c.mvp);
      std::memcpy(c.mv, mv, sizeof(mv));
      c.opts[0] = hm.srv ? 1.0f : 0.0f;
      D3D11_MAPPED_SUBRESOURCE map;
      if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) continue;
      std::memcpy(map.pData, &c, sizeof(c));
      ctx->Unmap(g_cb, 0);
      ID3D11ShaderResourceView* srv = hm.srv;
      ctx->PSSetShaderResources(0, 1, &srv);
      const UINT stride = sizeof(Vertex), offset = 0;
      ctx->IASetVertexBuffers(0, 1, &hm.vb, &stride, &offset);
      ctx->Draw(hm.vertexCount, 0);
    }
    ctx->IASetVertexBuffers(0, 1, &savedVb, &savedStride, &savedOffset);
    SafeRelease(savedVb);
  });
}

void DrawDebugUI() {
  ImGui::Checkbox("Controller models", &xr::GetSettings().controllerModels);
  for (int hand = 0; hand < 2; ++hand) {
    const HandModel& h = g_hands[hand];
    const char* side = hand ? "Right" : "Left";
    if (!h.cpu)
      ImGui::Text("%s: %s", side, g_loading[hand] ? "loading..." : "no controller");
    else if (!h.cpu->error.empty())
      ImGui::Text("%s: %s (%s)", side, h.cpu->name.c_str(), h.cpu->error.c_str());
    else
      ImGui::Text("%s: %s, %u triangles%s", side, h.cpu->name.c_str(), h.vertexCount / 3, h.srv ? "" : ", no texture");
  }
}

void Shutdown() {
  for (HandModel& h : g_hands) {
    ReleaseHand(h);
    h.cpu.reset();
    h.profile.clear();
  }
  SafeRelease(g_dsv);
  SafeRelease(g_depthTex);
  SafeRelease(g_vs);
  SafeRelease(g_ps);
  SafeRelease(g_layout);
  SafeRelease(g_cb);
  SafeRelease(g_samp);
  SafeRelease(g_raster);
  SafeRelease(g_depthState);
  g_device = nullptr;
  g_failed = false;
}

}  // namespace gdvr::controllers
