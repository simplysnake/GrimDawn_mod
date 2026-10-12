#include "mask.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>

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
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11ShaderResourceView* g_depthSrv = nullptr;
ID3D11Texture2D* g_depthSrvTex = nullptr;      // texture g_depthSrv was made for (not owned)
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11Texture2D* g_rtvTex = nullptr;           // back buffer g_rtv was made for (not owned)
bool g_failed = false;

constexpr char kShader[] = R"(
cbuffer Params : register(b0) {
  float4 right;   // w = tanX
  float4 up;      // w = tanY
  float4 fwd;     // w = near
  float4 origin;  // w = far
  float4 target;
  float4 fade;    // x inner, y outer (horizontal distance), z/w far-plane fade start/end (view depth)
  float4 color;   // a = strength
};
Texture2D<float> depthTex : register(t0);
Texture2D<float4> hudTex : register(t0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VS(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

float4 PS(VSOut i) : SV_Target {
  float d = depthTex.Load(int3(i.pos.xy, 0));
  if (d >= 0.99999) return float4(color.rgb, color.a);       // nothing drawn here: keep the void black
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
  return float4(color.rgb, a * color.a);
}

float4 PSHud(VSOut i) : SV_Target {
  float4 c = hudTex.Load(int3(i.pos.xy, 0));
  return float4(c.rgb, max(c.a, max(c.r, max(c.g, c.b))));
}
)";

struct alignas(16) Constants {
  float right[4], up[4], fwd[4], origin[4], target[4], fade[4], color[4];
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
  if (g_capturing && dsv && dsv != g_lastDsv) {
    g_lastDsv = dsv;
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
      D3D11_TEXTURE2D_DESC d;
      tex->GetDesc(&d);
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
  ID3DBlob* errors = nullptr;
  bool ok = SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mask", nullptr, nullptr, "VS", "vs_5_0", 0, 0,
                                 &vsBlob, &errors)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mask", nullptr, nullptr, "PS", "ps_5_0", 0, 0,
                                 &psBlob, &errors)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mask", nullptr, nullptr, "PSHud", "ps_5_0", 0, 0,
                                 &hudBlob, &errors));
  if (!ok) {
    Log("mask: shader compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
  } else {
    ok = SUCCEEDED(g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs)) &&
         SUCCEEDED(g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps)) &&
         SUCCEEDED(g_device->CreatePixelShader(hudBlob->GetBufferPointer(), hudBlob->GetBufferSize(), nullptr, &g_hudPs));
  }
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
  ID3D11ShaderResourceView* psSrv = nullptr;
  ID3D11Buffer* psCb = nullptr;
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
    c->PSGetShaderResources(0, 1, &psSrv);
    c->PSGetConstantBuffers(0, 1, &psCb);
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
    c->PSSetShaderResources(0, 1, &psSrv);
    c->PSSetConstantBuffers(0, 1, &psCb);
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
    SafeRelease(psSrv);
    SafeRelease(psCb);
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

bool FixHudAlpha(IDXGISwapChain* swapChain) {
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
    StateBackup backup;
    backup.Save(ctx);
    D3D11_VIEWPORT vp{0, 0, float(bbDesc.Width), float(bbDesc.Height), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);  // opaque: write rgb and the fixed alpha
    ctx->OMSetDepthStencilState(g_depthState, 0);
    g_origOMSetRenderTargets(ctx, 1, &g_rtv, nullptr);
    ctx->PSSetShaderResources(0, 1, &g_hudCopySrv);
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
  }
  bb->Release();
  return ok;
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
      ctx->Unmap(g_cb, 0);
    }

    StateBackup backup;
    backup.Save(ctx);
    D3D11_VIEWPORT vp{0, 0, float(bbDesc.Width), float(bbDesc.Height), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_raster);
    const float factor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(g_blend, factor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_depthState, 0);
    g_origOMSetRenderTargets(ctx, 1, &g_rtv, nullptr);
    ctx->PSSetShaderResources(0, 1, &g_depthSrv);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetShader(g_ps, nullptr, 0);
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
  }

  ctx->Release();
  bb->Release();
  return ok;
}

int InFlight() { return g_inFlight.load(); }

void Shutdown() {
  // The OMSetRenderTargets hook itself is removed by MH_Uninitialize; with g_capturing false and
  // the depth texture released it is a pass-through until then.
  g_capturing = false;
  g_capturingAllowed = false;
  SafeRelease(g_depthTex);
  SafeRelease(g_depthSrv);
  SafeRelease(g_rtv);
  SafeRelease(g_vs);
  SafeRelease(g_ps);
  SafeRelease(g_hudPs);
  SafeRelease(g_hudCopySrv);
  SafeRelease(g_hudCopy);
  SafeRelease(g_cb);
  SafeRelease(g_blend);
  SafeRelease(g_depthState);
  SafeRelease(g_raster);
  SafeRelease(g_device);
  g_depthSrvTex = nullptr;
  g_rtvTex = nullptr;
  g_lastDsv = nullptr;
}

}  // namespace gdvr::mask
