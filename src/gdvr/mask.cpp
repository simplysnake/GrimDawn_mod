#include "mask.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <MinHook.h>

#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>

#include "d3dtrace.h"
#include "log.h"

namespace gdvr::mask {

namespace {

constexpr int kOMSetRenderTargetsIndex = 33;  // ID3D11DeviceContext vtable slot

using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                      ID3D11DepthStencilView*);
OMSetRenderTargetsFn g_origOMSetRenderTargets = nullptr;
void* g_hookTarget = nullptr;

std::atomic<int> g_inFlight{0};
std::atomic<bool> g_capturing{false};
unsigned g_wantWidth = 0, g_wantHeight = 0;
bool g_capturingAllowed = true;  // false after Shutdown
int g_pick = -1;
int g_matchIndex = 0;  // distinct matching depth buffers seen in this capture
ID3D11DepthStencilView* g_lastDsv = nullptr;  // not owned; only compared
// The screen-sized depth buffers the scene renders into (owned references; the title screen clears them).
constexpr int kMaxSceneDsvs = 4;
ID3D11DepthStencilView* g_sceneDsvs[kMaxSceneDsvs] = {};
ID3D11Texture2D* g_depthTex = nullptr;        // owned
DXGI_FORMAT g_depthFormat = DXGI_FORMAT_UNKNOWN;

ID3D11Device* g_device = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11PixelShader* g_hudPs = nullptr;
ID3D11Texture2D* g_hudCopy = nullptr;
ID3D11ShaderResourceView* g_hudCopySrv = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blend = nullptr;
// Contact shadows: linear depth with a full mip chain (mips = the blur), per eye.
ID3D11PixelShader* g_linPs = nullptr;
ID3D11Texture2D* g_linTex = nullptr;
ID3D11RenderTargetView* g_linRtv = nullptr;
ID3D11ShaderResourceView* g_linSrv = nullptr;
ID3D11PixelShader* g_aoPs = nullptr;
ID3D11Texture2D* g_aoTex = nullptr;  // half size, full mip chain
ID3D11RenderTargetView* g_aoRtv = nullptr;
ID3D11ShaderResourceView* g_aoSrv = nullptr;
// Smoothed over time: the result (occlusion + depth, with mips for the blur) and each eye's copy of it.
ID3D11PixelShader* g_temporalPs = nullptr;
ID3D11Texture2D* g_aoOut = nullptr;
ID3D11RenderTargetView* g_aoOutRtv = nullptr;
ID3D11ShaderResourceView* g_aoOutSrv = nullptr;
ID3D11Texture2D* g_aoHist[2] = {};
ID3D11ShaderResourceView* g_aoHistSrv[2] = {};
struct PrevView {
  Vec3 right, up, forward, origin;
  float tanX = 1, tanY = 1;
  bool valid = false;
};
PrevView g_aoPrev[2];

// A mip-mapped float render target (`w` x `h`) for the contact shadows.
bool MakeMipTarget(UINT w, UINT h, ID3D11Texture2D*& tex, ID3D11RenderTargetView*& rtv, ID3D11ShaderResourceView*& srv,
                   DXGI_FORMAT format = DXGI_FORMAT_R16_FLOAT) {
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 0;  // full chain
  d.ArraySize = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  D3D11_RENDER_TARGET_VIEW_DESC rv{};
  rv.Format = d.Format;
  rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  return SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &tex)) && SUCCEEDED(g_device->CreateRenderTargetView(tex, &rv, &rtv)) &&
         SUCCEEDED(g_device->CreateShaderResourceView(tex, nullptr, &srv));
}

void ReleaseContactTextures() {
  for (int e = 0; e < 2; ++e) {
    if (g_aoHistSrv[e]) g_aoHistSrv[e]->Release();
    if (g_aoHist[e]) g_aoHist[e]->Release();
    g_aoHistSrv[e] = nullptr;
    g_aoHist[e] = nullptr;
    g_aoPrev[e].valid = false;
  }
  if (g_aoOutSrv) g_aoOutSrv->Release();
  if (g_aoOutRtv) g_aoOutRtv->Release();
  if (g_aoOut) g_aoOut->Release();
  g_aoOutSrv = nullptr;
  g_aoOutRtv = nullptr;
  g_aoOut = nullptr;
}

bool MakeContactTextures(UINT w, UINT h) {
  if (!MakeMipTarget(w, h, g_aoOut, g_aoOutRtv, g_aoOutSrv, DXGI_FORMAT_R16G16_FLOAT)) return false;
  for (int e = 0; e < 2; ++e) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_aoHist[e])) ||
        FAILED(g_device->CreateShaderResourceView(g_aoHist[e], nullptr, &g_aoHistSrv[e])))
      return false;
  }
  return true;
}
ID3D11SamplerState* g_sampClamp = nullptr;
ID3D11SamplerState* g_sampWrap = nullptr;
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11ShaderResourceView* g_depthSrv = nullptr;
ID3D11Texture2D* g_depthSrvTex = nullptr;      // texture g_depthSrv was made for (not owned)
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11Texture2D* g_rtvTex = nullptr;           // back buffer g_rtv was made for (not owned)
bool g_failed = false;

// Per-eye depth history (see mask.h)
ID3D11Texture2D* g_historyTarget = nullptr;   // the engine's depth copy (not owned)
ID3D11Texture2D* g_history[2] = {};           // our saved copy per eye (owned)
std::atomic<int> g_historyMode{2};
ID3D11DepthStencilView* g_historyDsv = nullptr;  // for clearing the engine's depth copy (mode 2)

constexpr char kShader[] = R"(
cbuffer Params : register(b0) {
  float4 right;   // w = tanX
  float4 up;      // w = tanY
  float4 fwd;     // w = near
  float4 origin;  // w = far
  float4 target;
  float4 fade;    // x inner, y outer (horizontal distance), z/w far-plane fade start/end (view depth)
  float4 color;   // a = strength
  float4 hud;     // HUD pass: x brightness, y opacity, z halo strength, w halo radius (px)
  float4 hud2;    // HUD pass: x alpha boost, y side-block shift (px, + = left), z outline, w key black
  float4 hud3;    // HUD pass: side block rect in px (x0, y0, x1, y1), as the game draws it
  float4 sky0;    // sky: x constellations present, y backdrop present, z key, w key softness
  float4 sky1;    // sky: xyz level forward, w band half width (rad)
  float4 sky2;    // sky: xyz level right, w sky on
  float4 sky3;    // sky: x band low, y band high (rad), z brightness, w opacity
  float4 sky4;    // sky: crop of the capture (left, top, right, bottom)
  float4 sky5;    // sky: x backdrop brightness, y dome mode, z dome rim elevation (rad)
  float4 ao;      // contact shadows: x strength (0 = off), y blur (mip level of the AO), w radius (game units)
  float4 prevR;   // contact shadows, this eye's previous frame: right (w tanX), up (w tanY), forward,
  float4 prevU;   // origin (w: 1 = history valid)
  float4 prevF;
  float4 prevO;
  float4 hud4;    // HUD pass: title screen, the character picture's rect in px (drawn in the scene instead)
};
Texture2D<float4> skyConst : register(t1);
Texture2D<float4> skyBack : register(t2);
SamplerState sampClamp : register(s0);
SamplerState sampWrap : register(s1);

static const float kPi = 3.14159265;

// The sky in a world direction: the backdrop panorama, with the constellation band on top.
float3 Sky(float3 dir) {
  float3 up = float3(0, 1, 0);
  float yaw = atan2(dot(dir, sky2.xyz), dot(dir, sky1.xyz));  // + = right of the forward direction
  float elev = asin(clamp(dot(dir, up), -1, 1));
  float3 c = float3(0, 0, 0);
  if (sky0.y > 0) c = skyBack.SampleLevel(sampWrap, float2(yaw / (2 * kPi) + 0.5, 0.5 - elev / kPi), 0).rgb * sky5.x;
  if (sky0.x > 0 && sky5.y > 0) {
    // The devotion map as a dome: its centre straight overhead, its rim at sky5.z all around,
    // its top in front of the game camera.
    float r = (kPi / 2 - elev) / (kPi / 2 - sky5.z) * 0.5;
    if (r < 0.5) {
      float2 uv = 0.5 + r * float2(-sin(yaw), -cos(yaw));  // seen from inside: east-west swapped, like a sky chart
      float4 s = skyConst.Sample(sampClamp, uv);
      float edge = saturate((0.5 - r) * 30);
      c = lerp(c, s.rgb * sky3.z, s.a * sky3.w * edge);
    }
  } else if (sky0.x > 0 && abs(yaw) < sky1.w && elev > sky3.x && elev < sky3.y) {
    float2 t = float2(yaw / sky1.w * 0.5 + 0.5, 1 - (elev - sky3.x) / (sky3.y - sky3.x));
    float4 s = skyConst.SampleLevel(sampClamp, lerp(sky4.xy, sky4.zw, t), 0);
    float lum = max(s.r, max(s.g, s.b));
    float a = smoothstep(sky0.z, sky0.z + sky0.w, lum) * sky3.w;
    float edge = saturate(min(min(t.x, 1 - t.x), min(t.y, 1 - t.y)) * 12);  // soft band edges
    c = lerp(c, s.rgb * sky3.z, a * edge);
  }
  return c;
}
Texture2D<float> depthTex : register(t0);
Texture2D<float4> hudTex : register(t0);
Texture2D<float> linDepth : register(t3);  // contact shadows: view depth / far (mip 1 = half size)
Texture2D<float> aoTex : register(t4);     // contact shadows: occlusion at half size, with mips (the blur)
Texture2D<float2> aoHist : register(t5);   // contact shadows: this eye's previous result (x occlusion, y depth / far)

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VS(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float4 PS(VSOut i) : SV_Target {
  float d = depthTex.Load(int3(i.pos.xy, 0));
  float2 ndc0 = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
  float3 dir = normalize(right.xyz * (ndc0.x * right.w) + up.xyz * (ndc0.y * up.w) + fwd.xyz);
  float3 voidColor = sky2.w > 0 ? Sky(dir) : color.rgb;
  if (d >= 0.99999) return float4(voidColor, sky2.w > 0 ? 1 : color.a);  // nothing drawn here: the void
  float n = fwd.w, f = origin.w;
  float z = n * f / (f - d * (f - n));                        // linear view depth
  float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
  float3 world = origin.xyz + right.xyz * (ndc.x * right.w * z) + up.xyz * (ndc.y * up.w * z) + fwd.xyz * z;
  float dist = length(world.xz - target.xz);
  if (color.r > 0.5) {  // debug: rings every 10 units (red at the fade start, blue at full black)
    float band = frac(dist / 10.0);
    float3 c = dist < fade.x ? float3(0, band, 0) : (dist < fade.y ? float3(1, 0, 0) : float3(0, 0, 1));
    return float4(c, 0.45);
  }
  float a = smoothstep(fade.x, fade.y, dist);
  a = max(a, smoothstep(fade.z, fade.w, z));

  // Contact shadows (PSAO), blurred: 9 taps over a mip of it (smooth, no blocky mip steps).
  float occl = 0;
  if (ao.x > 0) {
    float2 dims;
    aoTex.GetDimensions(dims.x, dims.y);
    float2 o = exp2(ao.y) * 1.25 / dims;
    float sum = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
      [unroll] for (int x = -1; x <= 1; ++x)
        sum += aoTex.SampleLevel(sampClamp, i.uv + float2(x, y) * o, max(ao.y - 1, 0)) * ((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1));
    occl = saturate(sum / 16 * 1.6) * ao.x;
  }
  // The fade (towards voidColor, alpha a) and the darkening in one blend: result = scene * (1 - A) * (1 - occl)
  // + voidColor * A.
  float A = a * color.a;
  float outA = A + occl - A * occl;
  return float4(voidColor * (A / max(outA, 1e-4)), outA);
}

// Contact shadows (ambient occlusion), at half size. Each point looks at a half-sphere of `ao.w` game units
// over its own surface: geometry inside it (a rock's side next to the ground, grass over its roots, a
// character's feet) darkens it; anything farther away (a tree in front of a distant wall) doesn't count.
// Uses this eye's own depth of this frame: nothing lags or slides. The same sample pattern everywhere (no
// per-pixel noise, which shimmered as the scene moved under it); the blur smooths it.
static const float3 kKernel[12] = {
  float3(0.53, 0.08, 0.30), float3(-0.29, 0.44, 0.22), float3(-0.41, -0.33, 0.35), float3(0.22, -0.55, 0.18),
  float3(0.71, 0.37, 0.41), float3(-0.66, 0.12, 0.52), float3(0.05, 0.78, 0.33), float3(0.31, -0.71, 0.47),
  float3(-0.84, -0.41, 0.24), float3(0.92, -0.18, 0.30), float3(-0.17, -0.93, 0.28), float3(0.12, 0.21, 0.95)};
float PSAO(VSOut i) : SV_Target {
  float f = origin.w;
  float zc = linDepth.SampleLevel(sampClamp, i.uv, 1);
  if (zc >= 0.999) return 0;  // the void
  float z = zc * f;
  float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
  float3 P = float3(ndc.x * right.w * z, ndc.y * up.w * z, z);  // view space: x right, y up, z forward
  float3 N = normalize(cross(ddy(P), ddx(P)));
  if (N.z > 0) N = -N;  // towards the camera
  float3 T = normalize(cross(N, abs(N.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0)));
  float3 T2 = T, B2 = cross(N, T);
  float radius = ao.w, occ = 0;
  [unroll] for (int k = 0; k < 12; ++k) {
    float3 S = P + (T2 * kKernel[k].x + B2 * kKernel[k].y + N * kKernel[k].z) * radius;
    if (S.z < 0.1) continue;
    float2 sn = float2(S.x / (S.z * right.w), S.y / (S.z * up.w));
    float sz = linDepth.SampleLevel(sampClamp, float2(sn.x * 0.5 + 0.5, 0.5 - sn.y * 0.5), 1) * f;
    float range = saturate(radius / max(abs(z - sz), 1e-3));  // only geometry near this point counts
    occ += (sz < S.z - 0.03 * radius ? 1.0 : 0.0) * range;
  }
  return occ / 12;
}

// Contact shadows, smoothing over time: each pixel finds where its world point was in this eye's previous
// frame (head turns and moves, walking, the camera) and blends with the result there. A pixel that was
// hidden or off-screen then (its depth doesn't match) starts fresh, so nothing smears or slides.
float2 PSTemporal(VSOut i) : SV_Target {
  float raw = aoTex.SampleLevel(sampClamp, i.uv, 0);
  float f = origin.w;
  float zc = linDepth.SampleLevel(sampClamp, i.uv, 1);
  if (zc >= 0.999) return float2(0, 1);
  float z = zc * f;
  float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
  float3 W = origin.xyz + right.xyz * (ndc.x * right.w * z) + up.xyz * (ndc.y * up.w * z) + fwd.xyz * z;
  float result = raw;
  if (prevO.w > 0) {
    float3 v = W - prevO.xyz;
    float pz = dot(v, prevF.xyz);
    if (pz > 0.1) {
      float2 pn = float2(dot(v, prevR.xyz) / (pz * prevR.w), dot(v, prevU.xyz) / (pz * prevU.w));
      if (all(abs(pn) < 0.995)) {
        float2 h = aoHist.SampleLevel(sampClamp, float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5), 0);
        if (abs(h.y * f - pz) < 0.04 * pz) result = lerp(raw, h.x, 0.88);
      }
    }
  }
  return float2(result, zc);
}

// Contact shadows, first step: linear view depth (/ far; the void = 1), mip-mapped afterwards.
float PSLinear(VSOut i) : SV_Target {
  float d = depthTex.Load(int3(i.pos.xy, 0));
  if (d >= 0.99999) return 1;
  float n = fwd.w, f = origin.w;
  return n / (f - d * (f - n));  // = view depth / far
}

// Some HUD elements don't write alpha; their colour (premultiplied over black) says how visible they are.
// The boost makes dark-but-drawn pixels solid instead of see-through (which looked additive).
// Key black: the alpha is ignored and dark parts fade out (the title screen's opaque black backdrop).
float Coverage(float4 c) {
  float l = max(c.r, max(c.g, c.b));
  if (hud2.w > 0) return smoothstep(0.03, 0.12, l);
  return saturate(max(c.a, l * hud2.x));
}

bool InRect(float2 p, float4 r) { return p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w; }

// The HUD as shown: the side block appears shifted left, and its original place is left empty.
float4 Hud(int2 p) {
  float4 moved = hud3 - float4(hud2.y, 0, hud2.y, 0);
  bool fromBlock = hud2.y != 0 && InRect(p, moved);
  bool vacated = hud2.y != 0 && !fromBlock && InRect(p, hud3);
  float4 here = hudTex.Load(int3(p, 0));
  // Whatever the game draws where the block is moved to (a menu, a dialog) stays on top of it.
  float4 block = hudTex.Load(int3(p + int2(hud2.y, 0), 0));
  bool showBlock = fromBlock && Coverage(here) < 0.05;
  return showBlock ? block : (vacated ? float4(0, 0, 0, 0) : here);
}

float4 PSHud(VSOut i) : SV_Target {
  int2 p = int2(i.pos.xy);
  if (hud2.w > 0 && hud4.z > hud4.x && InRect(p, hud4)) return float4(0, 0, 0, 0);
  if (hud2.z > 0) {  // outline: red where the game draws the side block, green where it is shown
    float4 moved = hud3 - float4(hud2.y, 0, hud2.y, 0);
    if (InRect(p, moved + float4(-3, -3, 3, 3)) && !InRect(p, moved + float4(3, 3, -3, -3))) return float4(0, 1, 0, 1);
    if (InRect(p, hud3 + float4(-3, -3, 3, 3)) && !InRect(p, hud3 + float4(3, 3, -3, -3))) return float4(1, 0, 0, 1);
  }
  float4 c = Hud(p);
  float a = Coverage(c);  // some HUD elements don't write alpha; colours are premultiplied over black
  // Soft dark halo: coverage of the neighbourhood (two rings), drawn as black behind the HUD.
  float halo = 0;
  if (hud.z > 0) {
    // 12 directions x 3 rings; outer rings count less, so the halo fades softly outwards.
    [unroll] for (int ring = 1; ring <= 3; ++ring) {
      float r = hud.w * ring / 3.0;
      float w = 1.0 - (ring - 1) * 0.3;
      [unroll] for (int k = 0; k < 12; ++k) {
        float2 dir = float2(cos(k * 0.5235988), sin(k * 0.5235988));
        halo = max(halo, w * Coverage(Hud(p + int2(round(dir * r)))));
      }
    }
    halo *= hud.z;
  }
  return float4(c.rgb * hud.x * hud.y, max(a, halo) * hud.y);
}
)";

struct alignas(16) Constants {
  float right[4], up[4], fwd[4], origin[4], target[4], fade[4], color[4], hud[4], hud2[4], hud3[4];
  float sky0[4], sky1[4], sky2[4], sky3[4], sky4[4], sky5[4], ao[4], prevR[4], prevU[4], prevF[4], prevO[4], hud4[4];
};

template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

DXGI_FORMAT SrvFormatFor(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
                                              ID3D11DepthStencilView* dsv) {
  ++g_inFlight;
  d3dtrace::OnSetRenderTargets(n, reinterpret_cast<void* const*>(rtvs), dsv);
  if (g_capturing && dsv && dsv != g_lastDsv) {
    g_lastDsv = dsv;
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
      D3D11_TEXTURE2D_DESC d;
      tex->GetDesc(&d);
      if (d.Width == g_wantWidth && d.Height == g_wantHeight) {  // remember every screen-sized one
        bool known = false;
        for (ID3D11DepthStencilView* v : g_sceneDsvs) known |= v == dsv;
        for (ID3D11DepthStencilView*& v : g_sceneDsvs)
          if (!known && !v) {
            v = dsv;
            v->AddRef();
            break;
          }
      }
      if ((d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && d.SampleDesc.Count == 1 && SrvFormatFor(d.Format) &&
          d.Width == g_wantWidth && d.Height == g_wantHeight && (g_pick < 0 || g_matchIndex++ == g_pick) &&
          tex != g_depthTex) {
        SafeRelease(g_depthTex);
        g_depthTex = tex;
        tex = nullptr;
        g_depthFormat = d.Format;
        static int logged = 0;
        if (logged++ < 4) Log("mask: scene depth %p %ux%u format %d", g_depthTex, d.Width, d.Height, int(d.Format));
      }
    }
    SafeRelease(tex);
    SafeRelease(res);
  }
  g_origOMSetRenderTargets(ctx, n, rtvs, dsv);
  --g_inFlight;
}

bool CreateResources() {
  ID3DBlob* vsBlob = nullptr;
  ID3DBlob* psBlob = nullptr;
  ID3DBlob* hudBlob = nullptr;
  ID3DBlob* linBlob = nullptr;
  ID3DBlob* aoBlob = nullptr;
  ID3DBlob* tBlob = nullptr;
  ID3DBlob* errors = nullptr;
  bool ok = SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mask", nullptr, nullptr, "VS", "vs_5_0", 0, 0,
                                 &vsBlob, &errors)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mask", nullptr, nullptr, "PS", "ps_5_0", 0, 0,
                                 &psBlob, &errors)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mask", nullptr, nullptr, "PSHud", "ps_5_0", 0, 0,
                                 &hudBlob, &errors)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mask", nullptr, nullptr, "PSLinear", "ps_5_0", 0, 0,
                                 &linBlob, &errors)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mask", nullptr, nullptr, "PSAO", "ps_5_0", 0, 0,
                                 &aoBlob, &errors)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mask", nullptr, nullptr, "PSTemporal", "ps_5_0", 0,
                                 0, &tBlob, &errors));
  if (!ok) {
    Log("mask: shader compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
  } else {
    ok = SUCCEEDED(g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs)) &&
         SUCCEEDED(g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps)) &&
         SUCCEEDED(g_device->CreatePixelShader(hudBlob->GetBufferPointer(), hudBlob->GetBufferSize(), nullptr, &g_hudPs)) &&
         SUCCEEDED(g_device->CreatePixelShader(linBlob->GetBufferPointer(), linBlob->GetBufferSize(), nullptr, &g_linPs)) &&
         SUCCEEDED(g_device->CreatePixelShader(aoBlob->GetBufferPointer(), aoBlob->GetBufferSize(), nullptr, &g_aoPs)) &&
         SUCCEEDED(g_device->CreatePixelShader(tBlob->GetBufferPointer(), tBlob->GetBufferSize(), nullptr, &g_temporalPs));
  }
  SafeRelease(tBlob);
  SafeRelease(linBlob);
  SafeRelease(aoBlob);
  SafeRelease(vsBlob);
  SafeRelease(psBlob);
  SafeRelease(hudBlob);
  SafeRelease(errors);
  if (!ok) return false;

  D3D11_BUFFER_DESC bd{sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                           D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
                           D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE};
  D3D11_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = FALSE;
  ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
  D3D11_RASTERIZER_DESC rs{};
  rs.FillMode = D3D11_FILL_SOLID;
  rs.CullMode = D3D11_CULL_NONE;
  rs.DepthClipEnable = TRUE;
  D3D11_SAMPLER_DESC samp{};
  samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp.MaxLOD = D3D11_FLOAT32_MAX;
  D3D11_SAMPLER_DESC wrap = samp;
  wrap.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
  if (FAILED(g_device->CreateSamplerState(&samp, &g_sampClamp)) || FAILED(g_device->CreateSamplerState(&wrap, &g_sampWrap)))
    return false;
  return SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &g_cb)) &&
         SUCCEEDED(g_device->CreateBlendState(&blend, &g_blend)) &&
         SUCCEEDED(g_device->CreateDepthStencilState(&ds, &g_depthState)) &&
         SUCCEEDED(g_device->CreateRasterizerState(&rs, &g_raster));
}

// Everything Apply changes, so the engine's (cached) state is untouched afterwards.
struct StateBackup {
  UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  ID3D11RasterizerState* raster = nullptr;
  ID3D11BlendState* blend = nullptr;
  float blendFactor[4];
  UINT sampleMask = 0;
  ID3D11DepthStencilState* depth = nullptr;
  UINT stencilRef = 0;
  // The game binds textures once and expects them to stay: every slot the mod's passes use is put back.
  static constexpr UINT kSrvs = 8, kSamplers = 4;
  ID3D11ShaderResourceView* psSrvs[kSrvs] = {};
  ID3D11SamplerState* psSamplers[kSamplers] = {};
  ID3D11Buffer* psCb = nullptr;
  ID3D11Buffer* vsCb = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11HullShader* hs = nullptr;
  ID3D11DomainShader* dsh = nullptr;
  ID3D11ClassInstance* psInst[256];
  ID3D11ClassInstance* vsInst[256];
  ID3D11ClassInstance* gsInst[256];
  UINT psInstCount = 256, vsInstCount = 256, gsInstCount = 256;
  D3D11_PRIMITIVE_TOPOLOGY topology{};
  ID3D11InputLayout* layout = nullptr;
  ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv = nullptr;

  void Save(ID3D11DeviceContext* c) {
    c->RSGetViewports(&viewportCount, viewports);
    c->RSGetState(&raster);
    c->OMGetBlendState(&blend, blendFactor, &sampleMask);
    c->OMGetDepthStencilState(&depth, &stencilRef);
    c->PSGetShaderResources(0, kSrvs, psSrvs);
    c->PSGetSamplers(0, kSamplers, psSamplers);
    c->PSGetConstantBuffers(0, 1, &psCb);
    c->VSGetConstantBuffers(0, 1, &vsCb);
    c->PSGetShader(&ps, psInst, &psInstCount);
    c->VSGetShader(&vs, vsInst, &vsInstCount);
    c->GSGetShader(&gs, gsInst, &gsInstCount);
    c->HSGetShader(&hs, nullptr, nullptr);
    c->DSGetShader(&dsh, nullptr, nullptr);
    c->IAGetPrimitiveTopology(&topology);
    c->IAGetInputLayout(&layout);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
  }

  void Restore(ID3D11DeviceContext* c) {
    c->RSSetViewports(viewportCount, viewports);
    c->RSSetState(raster);
    c->OMSetBlendState(blend, blendFactor, sampleMask);
    c->OMSetDepthStencilState(depth, stencilRef);
    c->PSSetShaderResources(0, kSrvs, psSrvs);
    c->PSSetSamplers(0, kSamplers, psSamplers);
    c->PSSetConstantBuffers(0, 1, &psCb);
    c->VSSetConstantBuffers(0, 1, &vsCb);
    c->PSSetShader(ps, psInst, psInstCount);
    c->VSSetShader(vs, vsInst, vsInstCount);
    c->GSSetShader(gs, gsInst, gsInstCount);
    c->HSSetShader(hs, nullptr, 0);
    c->DSSetShader(dsh, nullptr, 0);
    c->IASetPrimitiveTopology(topology);
    c->IASetInputLayout(layout);
    g_origOMSetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);

    SafeRelease(raster);
    SafeRelease(blend);
    SafeRelease(depth);
    for (auto*& v : psSrvs) SafeRelease(v);
    for (auto*& v : psSamplers) SafeRelease(v);
    SafeRelease(psCb);
    SafeRelease(vsCb);
    SafeRelease(ps);
    SafeRelease(vs);
    SafeRelease(gs);
    SafeRelease(hs);
    SafeRelease(dsh);
    for (UINT i = 0; i < psInstCount; ++i) SafeRelease(psInst[i]);
    for (UINT i = 0; i < vsInstCount; ++i) SafeRelease(vsInst[i]);
    for (UINT i = 0; i < gsInstCount; ++i) SafeRelease(gsInst[i]);
    SafeRelease(layout);
    for (auto*& r : rtvs) SafeRelease(r);
    SafeRelease(dsv);
  }
};

void Set4(float* out, Vec3 v, float w) {
  out[0] = v.x;
  out[1] = v.y;
  out[2] = v.z;
  out[3] = w;
}

}  // namespace

void InstallHook(ID3D11DeviceContext* context) {
  if (g_hookTarget) return;
  void** vtable = *reinterpret_cast<void***>(context);
  g_hookTarget = vtable[kOMSetRenderTargetsIndex];
  if (MH_CreateHook(g_hookTarget, reinterpret_cast<void*>(&HookOMSetRenderTargets),
                    reinterpret_cast<void**>(&g_origOMSetRenderTargets)) != MH_OK ||
      MH_EnableHook(g_hookTarget) != MH_OK) {
    Log("mask: failed to hook OMSetRenderTargets");
    g_hookTarget = nullptr;
    return;
  }
  Log("mask: OMSetRenderTargets hooked");
}

void BeginCapture(unsigned width, unsigned height, int pick) {
  if (!g_capturingAllowed) return;
  g_pick = pick;
  g_matchIndex = 0;
  g_wantWidth = width;
  g_wantHeight = height;
  g_lastDsv = nullptr;
  g_capturing = true;
}
void EndCapture() { g_capturing = false; }

// Lazily creates the shared device objects and an RTV for the back buffer `bb`.
bool EnsureDeviceAndRtv(ID3D11Texture2D* bb) {
  if (!g_device) {
    bb->GetDevice(&g_device);
    if (!CreateResources()) {
      g_failed = true;
      return false;
    }
  }
  if (g_rtvTex != bb) {
    SafeRelease(g_rtv);
    if (FAILED(g_device->CreateRenderTargetView(bb, nullptr, &g_rtv))) return false;
    g_rtvTex = bb;
  }
  return true;
}

// ---- Image meters: texture -> mip chain -> 1x1 average, read back a few frames later -----------
struct Meter {
  const char* name;
  float (*reduce)(const uint8_t* px, bool bgra);  // the 1x1 average pixel -> the value
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  UINT mips = 0;
  static constexpr int kSlots = 4;
  ID3D11Texture2D* staging[kSlots] = {};
  bool pending[kSlots] = {};
  int calls = 0;
  std::atomic<float> value{-1.0f};
  // Optional: read back a whole small mip level too (the HUD's alpha map for the pointer).
  int gridLevel = -1;
  bool gridBrightness = false;  // grid cells hold max(r, g, b) instead of alpha
  ID3D11Texture2D* gridStaging = nullptr;
  bool gridPending = false;
  UINT gridW = 0, gridH = 0;
  std::mutex gridMutex;
  std::vector<uint8_t> grid;  // alpha per grid cell, under gridMutex

  void Release() {
    SafeRelease(gridStaging);
    gridPending = false;
    SafeRelease(srv);
    SafeRelease(tex);
    for (auto*& st : staging) SafeRelease(st);
    for (bool& b : pending) b = false;
    mips = 0;
  }

  // Samples bb every 4th call. No stalls: results are read only once the GPU has finished them.
  void Sample(ID3D11Texture2D* bb) {
    if (calls++ % 4) return;
    D3D11_TEXTURE2D_DESC bbDesc;
    bb->GetDesc(&bbDesc);
    if (tex) {
      D3D11_TEXTURE2D_DESC d;
      tex->GetDesc(&d);
      if (d.Width != bbDesc.Width || d.Height != bbDesc.Height || d.Format != bbDesc.Format) Release();
    }
    if (!tex) {
      D3D11_TEXTURE2D_DESC d = bbDesc;
      d.MipLevels = 0;  // full chain down to 1x1
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      d.CPUAccessFlags = 0;
      d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
      D3D11_TEXTURE2D_DESC sd = bbDesc;
      sd.Width = sd.Height = 1;
      sd.MipLevels = 1;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      bool ok = SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &tex)) &&
                SUCCEEDED(g_device->CreateShaderResourceView(tex, nullptr, &srv));
      for (auto*& st : staging) ok = ok && SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &st));
      if (!ok) {
        Log("hud: %s meter unavailable", name);
        Release();
        return;
      }
      tex->GetDesc(&d);
      mips = d.MipLevels;
    }
    ID3D11DeviceContext* ctx = nullptr;
    g_device->GetImmediateContext(&ctx);
    const int slot = (calls / 4) % kSlots;
    const int read = (slot + 1) % kSlots;  // the oldest pending result
    if (pending[read]) {
      D3D11_MAPPED_SUBRESOURCE mapped;
      if (ctx->Map(staging[read], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) == S_OK) {
        const bool bgra = bbDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || bbDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        value = reduce(static_cast<const uint8_t*>(mapped.pData), bgra);
        ctx->Unmap(staging[read], 0);
        pending[read] = false;
      }
    }
    ctx->CopySubresourceRegion(tex, 0, 0, 0, 0, bb, 0, nullptr);
    ctx->GenerateMips(srv);
    ctx->CopySubresourceRegion(staging[slot], 0, 0, 0, 0, tex, mips - 1, nullptr);
    pending[slot] = true;
    if (gridLevel >= 0 && UINT(gridLevel) < mips) {
      if (!gridStaging) {
        D3D11_TEXTURE2D_DESC gd = bbDesc;
        gd.Width = std::max(1u, bbDesc.Width >> gridLevel);
        gd.Height = std::max(1u, bbDesc.Height >> gridLevel);
        gd.MipLevels = 1;
        gd.Usage = D3D11_USAGE_STAGING;
        gd.BindFlags = 0;
        gd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        gd.MiscFlags = 0;
        if (SUCCEEDED(g_device->CreateTexture2D(&gd, nullptr, &gridStaging))) {
          gridW = gd.Width;
          gridH = gd.Height;
        }
      }
      if (gridStaging && gridPending) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (ctx->Map(gridStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) == S_OK) {
          std::lock_guard lock(gridMutex);
          grid.resize(size_t(gridW) * gridH);
          for (UINT y = 0; y < gridH; ++y) {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            for (UINT x = 0; x < gridW; ++x)
              grid[size_t(y) * gridW + x] =
                  gridBrightness ? std::max(row[x * 4], std::max(row[x * 4 + 1], row[x * 4 + 2])) : row[x * 4 + 3];
          }
          ctx->Unmap(gridStaging, 0);
          gridPending = false;
        }
      }
      if (gridStaging && !gridPending) {
        ctx->CopySubresourceRegion(gridStaging, 0, 0, 0, 0, tex, gridLevel, nullptr);
        gridPending = true;
      }
    }
    ctx->Release();
  }
};

// Scene brightness of an eye image (luminance).
Meter g_sceneMeter{"brightness", [](const uint8_t* px, bool bgra) {
                     const float r = (bgra ? px[2] : px[0]) / 255.0f, g = px[1] / 255.0f,
                                 b = (bgra ? px[0] : px[2]) / 255.0f;
                     return 0.2126f * r + 0.7152f * g + 0.0722f * b;
                   }};
// How much of the HUD image is covered (average alpha after the HUD pass): jumps when a menu opens.
Meter g_hudMeter{"HUD coverage", [](const uint8_t* px, bool) { return px[3] / 255.0f; }};
// Title screen: brightness of the menu layer as the game drew it (finds the character picture).
Meter g_titleMeter{"title layer", [](const uint8_t* px, bool) { return px[1] / 255.0f; }};

void MeasureBrightness(IDXGISwapChain* swapChain) {
  if (!g_capturingAllowed || g_failed || !swapChain) return;
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
  if (EnsureDeviceAndRtv(bb)) g_sceneMeter.Sample(bb);
  bb->Release();
}

float SceneBrightness() { return g_sceneMeter.value.load(); }
float HudCoverage() { return g_hudMeter.value.load(); }

float HudRegionCoverage(float u0, float v0, float u1, float v1) {
  std::lock_guard lock(g_hudMeter.gridMutex);
  if (g_hudMeter.grid.empty()) return -1.0f;
  const UINT x0 = UINT(u0 * g_hudMeter.gridW), x1 = std::min(g_hudMeter.gridW, UINT(u1 * g_hudMeter.gridW));
  const UINT y0 = UINT(v0 * g_hudMeter.gridH), y1 = std::min(g_hudMeter.gridH, UINT(v1 * g_hudMeter.gridH));
  int drawn = 0, total = 0;
  for (UINT y = y0; y < y1; ++y)
    for (UINT x = x0; x < x1; ++x, ++total)
      if (g_hudMeter.grid[size_t(y) * g_hudMeter.gridW + x] > 77) ++drawn;  // alpha > 0.3
  return total ? float(drawn) / float(total) : -1.0f;
}

float HudAlphaAt(float u, float v) {
  std::lock_guard lock(g_hudMeter.gridMutex);
  if (g_hudMeter.grid.empty() || u < 0 || u > 1 || v < 0 || v > 1) return -1.0f;
  const UINT x = std::min(g_hudMeter.gridW - 1, UINT(u * g_hudMeter.gridW));
  const UINT y = std::min(g_hudMeter.gridH - 1, UINT(v * g_hudMeter.gridH));
  return g_hudMeter.grid[size_t(y) * g_hudMeter.gridW + x] / 255.0f;
}

bool FixHudAlpha(IDXGISwapChain* swapChain, const HudLook& look) {
  if (!g_capturingAllowed || g_failed || !g_origOMSetRenderTargets || !swapChain) return false;
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  bool ok = EnsureDeviceAndRtv(bb);
  D3D11_TEXTURE2D_DESC bbDesc;
  bb->GetDesc(&bbDesc);

  // Source copy of the HUD (a texture cannot be read and written in the same draw).
  if (ok && g_hudCopy) {
    D3D11_TEXTURE2D_DESC d;
    g_hudCopy->GetDesc(&d);
    if (d.Width != bbDesc.Width || d.Height != bbDesc.Height || d.Format != bbDesc.Format) {
      SafeRelease(g_hudCopySrv);
      SafeRelease(g_hudCopy);
    }
  }
  if (ok && !g_hudCopy) {
    D3D11_TEXTURE2D_DESC d = bbDesc;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    ok = SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &g_hudCopy)) &&
         SUCCEEDED(g_device->CreateShaderResourceView(g_hudCopy, nullptr, &g_hudCopySrv));
  }

  if (ok) {
    ID3D11DeviceContext* ctx = nullptr;
    g_device->GetImmediateContext(&ctx);
    ctx->CopyResource(g_hudCopy, bb);
    if (look.keyBlack) {
      g_titleMeter.gridLevel = 3;  // 2560x1440 -> 320x180
      g_titleMeter.gridBrightness = true;
      g_titleMeter.Sample(g_hudCopy);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      Constants& c = *static_cast<Constants*>(mapped.pData);
      std::memset(&c, 0, sizeof(c));
      c.hud[0] = look.brightness;
      c.hud[1] = look.opacity;
      c.hud[2] = look.halo;
      c.hud[3] = look.haloRadius;
      c.hud2[0] = look.alphaBoost;
      const float w = float(bbDesc.Width), h = float(bbDesc.Height);
      c.hud2[1] = std::round(look.sideShift * w);
      c.hud2[2] = look.sideOutline ? 1.0f : 0.0f;
      c.hud2[3] = look.keyBlack ? 1.0f : 0.0f;
      c.hud3[0] = std::round(w * (1 - look.sideWidth));
      c.hud3[1] = 0;
      c.hud3[2] = w;
      c.hud3[3] = std::round(h * look.sideHeight);
      if (look.keyBlack && look.cut[2] > look.cut[0])
        for (int k = 0; k < 4; ++k) c.hud4[k] = std::round(look.cut[k] * (k % 2 ? h : w));
      ctx->Unmap(g_cb, 0);
    }
    StateBackup backup;
    backup.Save(ctx);
    D3D11_VIEWPORT vp{0, 0, float(bbDesc.Width), float(bbDesc.Height), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);  // opaque: write rgb and the fixed alpha
    ctx->OMSetDepthStencilState(g_depthState, 0);
    g_origOMSetRenderTargets(ctx, 1, &g_rtv, nullptr);
    ctx->PSSetShaderResources(0, 1, &g_hudCopySrv);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetShader(g_hudPs, nullptr, 0);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    backup.Restore(ctx);
    ctx->Release();
    g_hudMeter.gridLevel = 4;  // 2560x1440 -> 160x90 cells for the pointer's HUD hit test
    g_hudMeter.Sample(bb);
  }
  bb->Release();
  return ok;
}

namespace {

// ---- Title screen: the character picture standing in the scene ------------------------------------
// Scene depth at one pixel (read back a frame later), and the picture drawn as a card in an eye image.
constexpr char kCutoutShader[] = R"(
cbuffer C : register(b0) { float4 c01; float4 c23; float4 rect; float4 probe; };  // corners NDC: BL BR TR TL
Texture2D<float4> pic : register(t0);
Texture2D<float> depth : register(t1);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
  uint k = id < 3 ? id : (id == 3 ? 0 : (id == 4 ? 2 : 3));  // BL BR TR, BL TR TL
  float2 ndc = k == 0 ? c01.xy : (k == 1 ? c01.zw : (k == 2 ? c23.xy : c23.zw));
  VSOut o;
  o.pos = float4(ndc, 0, 1);
  o.uv = float2((k == 1 || k == 2) ? rect.z : rect.x, k >= 2 ? rect.y : rect.w);
  return o;
}
float4 PS(VSOut i) : SV_Target {
  float4 c = pic.SampleLevel(samp, i.uv, 0);
  return float4(c.rgb, smoothstep(0.03, 0.12, max(c.r, max(c.g, c.b))));  // the title layer's black is see-through
}
float4 VSProbe(uint id : SV_VertexID) : SV_Position { return float4((id << 1) & 2 ? 3 : -1, id & 2 ? -3 : 1, 0, 1); }
float PSProbe() : SV_Target { return depth.Load(int3(probe.xy, 0)); }
)";
ID3D11VertexShader* g_cutVs = nullptr;
ID3D11PixelShader* g_cutPs = nullptr;
ID3D11VertexShader* g_probeVs = nullptr;
ID3D11PixelShader* g_probePs = nullptr;
ID3D11Buffer* g_cutCb = nullptr;
ID3D11Texture2D* g_probeTex = nullptr;   // 1x1 R32F
ID3D11RenderTargetView* g_probeRtv = nullptr;
ID3D11Texture2D* g_probeStaging = nullptr;
bool g_probePending = false;
float g_probeValue = -1;
bool g_cutFailed = false;

void ReleaseCutout() {
  SafeRelease(g_cutVs);
  SafeRelease(g_cutPs);
  SafeRelease(g_probeVs);
  SafeRelease(g_probePs);
  SafeRelease(g_cutCb);
  SafeRelease(g_probeRtv);
  SafeRelease(g_probeTex);
  SafeRelease(g_probeStaging);
  g_probePending = false;
  g_probeValue = -1;
}

bool EnsureCutout() {
  if (g_cutPs) return true;
  if (g_cutFailed || !g_device) return false;
  ID3DBlob *vs = nullptr, *ps = nullptr, *pvs = nullptr, *pps = nullptr, *err = nullptr;
  auto compile = [&](const char* entry, const char* target, ID3DBlob** out) {
    return SUCCEEDED(D3DCompile(kCutoutShader, sizeof(kCutoutShader) - 1, "gdvr_cutout", nullptr, nullptr, entry, target, 0,
                                0, out, &err));
  };
  bool ok = compile("VS", "vs_5_0", &vs) && compile("PS", "ps_5_0", &ps) && compile("VSProbe", "vs_5_0", &pvs) &&
            compile("PSProbe", "ps_5_0", &pps);
  if (!ok && err) Log("cutout: shader: %s", static_cast<const char*>(err->GetBufferPointer()));
  ok = ok && SUCCEEDED(g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_cutVs)) &&
       SUCCEEDED(g_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_cutPs)) &&
       SUCCEEDED(g_device->CreateVertexShader(pvs->GetBufferPointer(), pvs->GetBufferSize(), nullptr, &g_probeVs)) &&
       SUCCEEDED(g_device->CreatePixelShader(pps->GetBufferPointer(), pps->GetBufferSize(), nullptr, &g_probePs));
  SafeRelease(vs);
  SafeRelease(ps);
  SafeRelease(pvs);
  SafeRelease(pps);
  SafeRelease(err);
  D3D11_BUFFER_DESC bd{64, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = 1;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R32_FLOAT;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  D3D11_TEXTURE2D_DESC sd = td;
  sd.BindFlags = 0;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ok = ok && SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &g_cutCb)) && SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &g_probeTex)) &&
       SUCCEEDED(g_device->CreateRenderTargetView(g_probeTex, nullptr, &g_probeRtv)) &&
       SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &g_probeStaging));
  if (!ok) {
    Log("cutout: unavailable");
    g_cutFailed = true;
    ReleaseCutout();
  }
  return ok;
}

bool EnsureDepthSrv() {
  if (!g_depthTex) return false;
  if (g_depthSrv && g_depthSrvTex == g_depthTex) return true;
  SafeRelease(g_depthSrv);
  D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = SrvFormatFor(g_depthFormat);
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  g_depthSrvTex = g_depthTex;
  return SUCCEEDED(g_device->CreateShaderResourceView(g_depthTex, &sd, &g_depthSrv));
}

void SetCutoutConstants(ID3D11DeviceContext* ctx, const float c[16]) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(g_cutCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    std::memcpy(m.pData, c, 64);
    ctx->Unmap(g_cutCb, 0);
  }
}

}  // namespace

void ProbeDepth(float px, float py) {
  if (!g_capturingAllowed || g_failed || !EnsureCutout() || !EnsureDepthSrv()) return;
  ID3D11DeviceContext* ctx = nullptr;
  g_device->GetImmediateContext(&ctx);
  if (g_probePending) {  // the previous request
    D3D11_MAPPED_SUBRESOURCE m;
    if (ctx->Map(g_probeStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
      g_probeValue = *static_cast<const float*>(m.pData);
      ctx->Unmap(g_probeStaging, 0);
      g_probePending = false;
    }
  }
  if (!g_probePending) {
    const float c[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, std::floor(px), std::floor(py), 0, 0};
    SetCutoutConstants(ctx, c);
    StateBackup backup;
    backup.Save(ctx);
    const D3D11_VIEWPORT vp{0, 0, 1, 1, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_depthState, 0);
    g_origOMSetRenderTargets(ctx, 1, &g_probeRtv, nullptr);
    ID3D11ShaderResourceView* srvs[2] = {nullptr, g_depthSrv};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetConstantBuffers(0, 1, &g_cutCb);
    ctx->VSSetShader(g_probeVs, nullptr, 0);
    ctx->PSSetShader(g_probePs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {};
    ctx->PSSetShaderResources(0, 2, none);
    backup.Restore(ctx);
    ctx->CopyResource(g_probeStaging, g_probeTex);
    g_probePending = true;
  }
  ctx->Release();
}

float ProbedDepth() { return g_probeValue; }

void ClearSceneDepth(ID3D11DeviceContext* ctx) {
  for (ID3D11DepthStencilView*& v : g_sceneDsvs) {
    if (!v) continue;
    ID3D11Resource* res = nullptr;
    v->GetResource(&res);
    D3D11_TEXTURE2D_DESC d{};
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) tex->GetDesc(&d);
    SafeRelease(tex);
    SafeRelease(res);
    if (d.Width != g_wantWidth || d.Height != g_wantHeight) {  // resized since: forget it
      SafeRelease(v);
      continue;
    }
    ctx->ClearDepthStencilView(v, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
  }
}

bool TitleCharacterRect(float rect[4]) {
  std::lock_guard lock(g_titleMeter.gridMutex);
  const auto& g = g_titleMeter.grid;
  const UINT w = g_titleMeter.gridW, h = g_titleMeter.gridH;
  if (g.empty() || !w || !h) return false;
  // The middle of the screen, between the logo (top) and the buttons (bottom).
  const UINT x0 = UINT(w * 0.30f), x1 = UINT(w * 0.70f), y0 = UINT(h * 0.24f), y1 = UINT(h * 0.89f);
  UINT minX = w, minY = h, maxX = 0, maxY = 0, count = 0;
  for (UINT y = y0; y < y1; ++y)
    for (UINT x = x0; x < x1; ++x)
      if (g[size_t(y) * w + x] > 20) {
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        ++count;
      }
  if (count < 40) return false;
  rect[0] = float(minX) / w - 0.01f;
  rect[1] = float(minY) / h - 0.01f;
  rect[2] = float(maxX + 1) / w + 0.01f;
  rect[3] = float(maxY + 1) / h + 0.01f;
  return true;
}

void DrawCutout(IDXGISwapChain* swapChain, const EyeCam& cam, const Vec3 corners[4], const float rect[4]) {
  if (!g_capturingAllowed || g_failed || !g_hudCopySrv || !swapChain || !EnsureCutout()) return;
  float c[16];
  for (int k = 0; k < 4; ++k) {
    const Vec3 d = corners[k] - cam.origin;
    const float z = Dot(d, cam.forward);
    if (z <= cam.nearPlane) return;
    c[k * 2] = Dot(d, cam.right) / (z * cam.tanX);
    c[k * 2 + 1] = Dot(d, cam.up) / (z * cam.tanY);
  }
  for (int k = 0; k < 4; ++k) c[8 + k] = rect[k];
  c[12] = c[13] = c[14] = c[15] = 0;
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
  if (EnsureDeviceAndRtv(bb)) {
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    ID3D11DeviceContext* ctx = nullptr;
    g_device->GetImmediateContext(&ctx);
    SetCutoutConstants(ctx, c);
    StateBackup backup;
    backup.Save(ctx);
    const D3D11_VIEWPORT vp{0, 0, float(bd.Width), float(bd.Height), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    const float factor[4] = {};
    ctx->OMSetBlendState(g_blend, factor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_depthState, 0);
    g_origOMSetRenderTargets(ctx, 1, &g_rtv, nullptr);
    ctx->PSSetShaderResources(0, 1, &g_hudCopySrv);
    ctx->PSSetSamplers(0, 1, &g_sampClamp);
    ctx->PSSetConstantBuffers(0, 1, &g_cutCb);
    ctx->VSSetConstantBuffers(0, 1, &g_cutCb);
    ctx->VSSetShader(g_cutVs, nullptr, 0);
    ctx->PSSetShader(g_cutPs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(6, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    backup.Restore(ctx);
    ctx->Release();
  }
  bb->Release();
}

bool Apply(IDXGISwapChain* swapChain, const Params& p) {
  if (!g_capturingAllowed || g_failed || !g_depthTex || !g_origOMSetRenderTargets || !swapChain) return false;

  ID3D11Texture2D* bb = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  if (!EnsureDeviceAndRtv(bb)) {
    bb->Release();
    return false;
  }
  ID3D11DeviceContext* ctx = nullptr;
  g_device->GetImmediateContext(&ctx);

  D3D11_TEXTURE2D_DESC bbDesc, depthDesc;
  bb->GetDesc(&bbDesc);
  g_depthTex->GetDesc(&depthDesc);
  bool ok = bbDesc.Width == depthDesc.Width && bbDesc.Height == depthDesc.Height;

  if (ok && g_depthSrvTex != g_depthTex) {
    SafeRelease(g_depthSrv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = SrvFormatFor(g_depthFormat);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ok = SUCCEEDED(g_device->CreateShaderResourceView(g_depthTex, &sd, &g_depthSrv));
    g_depthSrvTex = g_depthTex;
  }

  if (ok) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      Constants& c = *static_cast<Constants*>(mapped.pData);
      Set4(c.right, p.right, p.tanX);
      Set4(c.up, p.up, p.tanY);
      Set4(c.fwd, p.forward, p.nearPlane);
      Set4(c.origin, p.origin, p.farPlane);
      Set4(c.target, p.target, 0);
      c.fade[0] = p.inner;
      c.fade[1] = p.outer;
      c.fade[2] = p.farPlane * p.depthStart;
      c.fade[3] = p.farPlane * 0.98f;
      c.color[0] = p.debug ? 1.0f : 0.0f;
      c.color[1] = c.color[2] = 0;
      c.color[3] = p.strength;
      const bool haveConst = p.skyConstellations != nullptr, haveBack = p.skyBackdrop != nullptr;
      c.sky0[0] = haveConst ? 1.0f : 0.0f;
      c.sky0[1] = haveBack ? 1.0f : 0.0f;
      c.sky0[2] = p.key;
      c.sky0[3] = std::max(0.001f, p.soft);
      Set4(c.sky1, p.skyForward, p.bandHalfWidth);
      Set4(c.sky2, p.skyRight, (haveConst || haveBack) && !p.debug ? 1.0f : 0.0f);
      c.sky3[0] = p.bandLow;
      c.sky3[1] = p.bandHigh;
      c.sky3[2] = p.brightness;
      c.sky3[3] = p.opacity;
      for (int k = 0; k < 4; ++k) c.sky4[k] = p.crop[k];
      c.sky5[0] = p.backdropBrightness;
      c.sky5[1] = p.dome ? 1.0f : 0.0f;
      c.sky5[2] = p.domeHorizon;
      c.ao[0] = p.debug ? 0.0f : p.contactStrength;
      c.ao[1] = p.contactSoftness;
      c.ao[3] = p.contactSize;
      const PrevView& pv = g_aoPrev[p.eye & 1];
      Set4(c.prevR, pv.right, pv.tanX);
      Set4(c.prevU, pv.up, pv.tanY);
      Set4(c.prevF, pv.forward, 0);
      Set4(c.prevO, pv.origin, pv.valid ? 1.0f : 0.0f);
      ctx->Unmap(g_cb, 0);
    }

    // Contact shadows: this eye's linear depth, then its mips (the blurred surroundings).
    bool contact = p.contactStrength > 0 && !p.debug && g_linPs && g_aoPs;
    if (contact && g_linTex) {
      D3D11_TEXTURE2D_DESC d;
      g_linTex->GetDesc(&d);
      if (d.Width != depthDesc.Width || d.Height != depthDesc.Height) {
        SafeRelease(g_linSrv);
        SafeRelease(g_linRtv);
        SafeRelease(g_linTex);
        SafeRelease(g_aoSrv);
        SafeRelease(g_aoRtv);
        SafeRelease(g_aoTex);
        ReleaseContactTextures();
      }
    }
    if (contact && !g_linTex) {
      const UINT w = depthDesc.Width, h = depthDesc.Height;
      if (!MakeMipTarget(w, h, g_linTex, g_linRtv, g_linSrv) ||
          !MakeMipTarget(std::max(1u, w / 2), std::max(1u, h / 2), g_aoTex, g_aoRtv, g_aoSrv) ||
          !MakeContactTextures(std::max(1u, w / 2), std::max(1u, h / 2))) {
        ReleaseContactTextures();
        Log("mask: contact shadows unavailable (textures)");
        SafeRelease(g_linSrv);
        SafeRelease(g_linRtv);
        SafeRelease(g_linTex);
        SafeRelease(g_aoSrv);
        SafeRelease(g_aoRtv);
        SafeRelease(g_aoTex);
        SafeRelease(g_linPs);  // don't try again
      }
    }
    contact = contact && g_linSrv && g_aoSrv && g_aoOutSrv && g_temporalPs;
    if (!contact) g_aoPrev[0].valid = g_aoPrev[1].valid = false;  // stale when it comes back on

    StateBackup backup;
    backup.Save(ctx);
    D3D11_VIEWPORT vp{0, 0, float(bbDesc.Width), float(bbDesc.Height), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    ctx->OMSetDepthStencilState(g_depthState, 0);
    ID3D11SamplerState* samplers[2] = {g_sampClamp, g_sampWrap};
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    if (contact) {
      // 1. linear depth (full size) and its mips; 2. occlusion at half size and its mips (the blur).
      ID3D11RenderTargetView* noRtv = nullptr;
      ID3D11ShaderResourceView* noSrv = nullptr;
      ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
      g_origOMSetRenderTargets(ctx, 1, &g_linRtv, nullptr);
      ctx->PSSetShaderResources(0, 1, &g_depthSrv);
      ctx->PSSetShader(g_linPs, nullptr, 0);
      ctx->Draw(3, 0);
      g_origOMSetRenderTargets(ctx, 1, &noRtv, nullptr);
      ctx->GenerateMips(g_linSrv);
      const D3D11_VIEWPORT half{0, 0, float(std::max(1u, bbDesc.Width / 2)), float(std::max(1u, bbDesc.Height / 2)), 0, 1};
      ctx->RSSetViewports(1, &half);
      g_origOMSetRenderTargets(ctx, 1, &g_aoRtv, nullptr);
      ctx->PSSetShaderResources(3, 1, &g_linSrv);
      ctx->PSSetShader(g_aoPs, nullptr, 0);
      ctx->Draw(3, 0);
      g_origOMSetRenderTargets(ctx, 1, &noRtv, nullptr);
      // 3. smoothed over time with this eye's previous result, then its mips (the blur) and a copy for next time
      const int e = p.eye & 1;
      g_origOMSetRenderTargets(ctx, 1, &g_aoOutRtv, nullptr);
      ID3D11ShaderResourceView* tin[3] = {g_linSrv, g_aoSrv, g_aoHistSrv[e]};
      ctx->PSSetShaderResources(3, 3, tin);
      ctx->PSSetShader(g_temporalPs, nullptr, 0);
      ctx->Draw(3, 0);
      g_origOMSetRenderTargets(ctx, 1, &noRtv, nullptr);
      ID3D11ShaderResourceView* none3[3] = {};
      ctx->PSSetShaderResources(3, 3, none3);
      ctx->GenerateMips(g_aoOutSrv);
      ctx->CopySubresourceRegion(g_aoHist[e], 0, 0, 0, 0, g_aoOut, 0, nullptr);
      g_aoPrev[e] = {p.right, p.up, p.forward, p.origin, p.tanX, p.tanY, true};
      ctx->RSSetViewports(1, &vp);
    }
    const float factor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(g_blend, factor, 0xFFFFFFFF);
    g_origOMSetRenderTargets(ctx, 1, &g_rtv, nullptr);
    ID3D11ShaderResourceView* srvs[5] = {g_depthSrv, static_cast<ID3D11ShaderResourceView*>(p.skyConstellations),
                                         static_cast<ID3D11ShaderResourceView*>(p.skyBackdrop), nullptr,
                                         contact ? g_aoOutSrv : nullptr};
    ctx->PSSetShaderResources(0, 5, srvs);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[5] = {};
    ctx->PSSetShaderResources(0, 5, none);
    backup.Restore(ctx);
  }

  ctx->Release();
  bb->Release();
  return ok;
}

void OnCopyResource(void* dstPtr, void* srcPtr) {
  auto* dst = static_cast<ID3D11Resource*>(dstPtr);
  auto* src = static_cast<ID3D11Resource*>(srcPtr);
  if (!dst || !src || dst == g_historyTarget) return;
  ID3D11Texture2D *d = nullptr, *s = nullptr;
  if (SUCCEEDED(dst->QueryInterface(IID_PPV_ARGS(&d))) && SUCCEEDED(src->QueryInterface(IID_PPV_ARGS(&s)))) {
    D3D11_TEXTURE2D_DESC dd, sd;
    d->GetDesc(&dd);
    s->GetDesc(&sd);
    // A screen-sized depth copied into another screen-sized depth during an eye render.
    if (dd.Width == g_wantWidth && dd.Height == g_wantHeight && SrvFormatFor(dd.Format) && sd.Format == dd.Format &&
        (sd.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
      g_historyTarget = d;  // engine-owned; it lives as long as its render targets
      SafeRelease(g_historyDsv);
      SafeRelease(g_history[0]);
      SafeRelease(g_history[1]);
      Log("depth history: engine depth copy %p (%ux%u f%d)", static_cast<void*>(d), dd.Width, dd.Height, int(dd.Format));
    }
  }
  SafeRelease(d);
  SafeRelease(s);
}

DXGI_FORMAT DsvFormatFor(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

void RestoreEyeHistory(int eye) {
  if (!g_historyTarget) return;
  if (g_historyMode == 2) {
    ID3D11Device* dev = nullptr;
    g_historyTarget->GetDevice(&dev);
    if (!g_historyDsv) {
      D3D11_TEXTURE2D_DESC d;
      g_historyTarget->GetDesc(&d);
      D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
      vd.Format = DsvFormatFor(d.Format);
      vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
      if (vd.Format == DXGI_FORMAT_UNKNOWN || FAILED(dev->CreateDepthStencilView(g_historyTarget, &vd, &g_historyDsv))) {
        Log("depth history: cannot clear the engine depth copy; falling back to per-eye history");
        g_historyMode = 1;
      }
    }
    if (g_historyDsv) {
      ID3D11DeviceContext* ctx = nullptr;
      dev->GetImmediateContext(&ctx);
      ctx->ClearDepthStencilView(g_historyDsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
      ctx->Release();
    }
    dev->Release();
    return;
  }
  if (g_historyMode != 1 || !g_history[eye & 1]) return;
  ID3D11Device* dev = nullptr;
  g_historyTarget->GetDevice(&dev);
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  ctx->CopyResource(g_historyTarget, g_history[eye & 1]);
  ctx->Release();
  dev->Release();
}

void SaveEyeHistory(int eye) {
  if (g_historyMode != 1 || !g_historyTarget) return;
  ID3D11Device* dev = nullptr;
  g_historyTarget->GetDevice(&dev);
  if (!g_history[eye & 1]) {
    D3D11_TEXTURE2D_DESC d;
    g_historyTarget->GetDesc(&d);
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // only used as a copy source/destination
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_history[eye & 1]))) {
      Log("depth history: could not create texture");
      g_historyMode = 0;
      dev->Release();
      return;
    }
  }
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  ctx->CopyResource(g_history[eye & 1], g_historyTarget);
  ctx->Release();
  dev->Release();
}

void SetDepthHistoryMode(int mode) { g_historyMode = mode; }
int DepthHistoryMode() { return g_historyMode; }

int InFlight() { return g_inFlight.load(); }

void Shutdown() {
  // The OMSetRenderTargets hook itself is removed by MH_Uninitialize; with g_capturing false and
  // the depth texture released it is a pass-through until then.
  g_capturing = false;
  g_capturingAllowed = false;
  SafeRelease(g_depthTex);
  SafeRelease(g_history[0]);
  SafeRelease(g_history[1]);
  SafeRelease(g_historyDsv);
  g_historyTarget = nullptr;
  SafeRelease(g_depthSrv);
  SafeRelease(g_rtv);
  SafeRelease(g_vs);
  SafeRelease(g_ps);
  SafeRelease(g_hudPs);
  g_sceneMeter.Release();
  g_hudMeter.Release();
  for (ID3D11DepthStencilView*& v : g_sceneDsvs) SafeRelease(v);
  g_titleMeter.Release();
  ReleaseCutout();
  g_sceneMeter.value = -1.0f;
  g_hudMeter.value = -1.0f;
  SafeRelease(g_hudCopySrv);
  SafeRelease(g_hudCopy);
  SafeRelease(g_cb);
  SafeRelease(g_blend);
  SafeRelease(g_linPs);
  SafeRelease(g_linSrv);
  SafeRelease(g_linRtv);
  SafeRelease(g_linTex);
  SafeRelease(g_aoPs);
  SafeRelease(g_aoSrv);
  SafeRelease(g_aoRtv);
  SafeRelease(g_aoTex);
  ReleaseContactTextures();
  SafeRelease(g_temporalPs);
  SafeRelease(g_sampClamp);
  SafeRelease(g_sampWrap);
  SafeRelease(g_depthState);
  SafeRelease(g_raster);
  SafeRelease(g_device);
  g_depthSrvTex = nullptr;
  g_rtvTex = nullptr;
  g_lastDsv = nullptr;
}

}  // namespace gdvr::mask
