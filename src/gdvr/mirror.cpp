#include "mirror.h"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"

namespace gdvr::mirror {

namespace {

// VS + PSEye: full-screen triangle, the eye image through a centred crop. VSPanel + PSHud: one HUD panel as
// a quad whose corners arrive in clip space (behind the eye is clipped by the hardware); the HUD's colour is
// already scaled by its opacity, so it blends as premultiplied alpha.
constexpr char kShader[] = R"(
cbuffer Mirror : register(b0) {
  float4 crop;        // xy: centre-relative scale of the eye image's uv
  float4 corner[4];   // panel corners in clip space: top left, top right, bottom left, bottom right
  float4 uvRect;      // the panel's rectangle of the HUD image
};
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
VSOut VSPanel(uint id : SV_VertexID) {
  VSOut o;
  o.pos = corner[id];
  o.uv = float2(id & 1 ? uvRect.z : uvRect.x, id & 2 ? uvRect.w : uvRect.y);
  return o;
}
float4 PSEye(VSOut i) : SV_Target {
  return float4(tex.SampleLevel(samp, 0.5 + (i.uv - 0.5) * crop.xy, 0).rgb, 1);
}
float4 PSHud(VSOut i) : SV_Target { return tex.SampleLevel(samp, i.uv, 0); }
)";

struct Constants {
  float crop[4];
  float corner[4][4];
  float uvRect[4];
};

ID3D11VertexShader* g_vs = nullptr;
ID3D11VertexShader* g_vsPanel = nullptr;
ID3D11PixelShader* g_psEye = nullptr;
ID3D11PixelShader* g_psHud = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11BlendState* g_opaque = nullptr;
ID3D11BlendState* g_premul = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_noDepth = nullptr;
bool g_failed = false;

// The kept eye and a copy of the HUD (a texture can't be read and drawn into at once).
struct Copy {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
};
Copy g_eye, g_hud;
bool g_haveEye = false;

template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

void Release(Copy& c) {
  SafeRelease(c.srv);
  SafeRelease(c.tex);
}

bool CreatePipeline(ID3D11Device* device) {
  if (g_vs) return true;
  if (g_failed) return false;
  ID3DBlob *vs = nullptr, *panel = nullptr, *eye = nullptr, *hud = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mirror", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mirror", nullptr, nullptr, "VSPanel", "vs_5_0", 0, 0, &panel, &err)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mirror", nullptr, nullptr, "PSEye", "ps_5_0", 0, 0, &eye, &err)) &&
            SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "gdvr_mirror", nullptr, nullptr, "PSHud", "ps_5_0", 0, 0, &hud, &err));
  if (!ok && err) Log("mirror: shader: %s", static_cast<const char*>(err->GetBufferPointer()));
  ok = ok && SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
       SUCCEEDED(device->CreateVertexShader(panel->GetBufferPointer(), panel->GetBufferSize(), nullptr, &g_vsPanel)) &&
       SUCCEEDED(device->CreatePixelShader(eye->GetBufferPointer(), eye->GetBufferSize(), nullptr, &g_psEye)) &&
       SUCCEEDED(device->CreatePixelShader(hud->GetBufferPointer(), hud->GetBufferSize(), nullptr, &g_psHud));
  SafeRelease(vs);
  SafeRelease(panel);
  SafeRelease(eye);
  SafeRelease(hud);
  SafeRelease(err);
  D3D11_BUFFER_DESC bd{sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  D3D11_BLEND_DESC opaque{};
  opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  D3D11_BLEND_DESC premul = opaque;
  auto& rt = premul.RenderTarget[0];
  rt.BlendEnable = TRUE;
  rt.SrcBlend = D3D11_BLEND_ONE;
  rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  rt.BlendOp = D3D11_BLEND_OP_ADD;
  rt.SrcBlendAlpha = D3D11_BLEND_ONE;
  rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;  // clips the part of a panel that is behind the eye
  D3D11_DEPTH_STENCIL_DESC dd{};
  ok = ok && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_cb)) && SUCCEEDED(device->CreateSamplerState(&sd, &g_sampler)) &&
       SUCCEEDED(device->CreateBlendState(&opaque, &g_opaque)) && SUCCEEDED(device->CreateBlendState(&premul, &g_premul)) &&
       SUCCEEDED(device->CreateRasterizerState(&rd, &g_raster)) && SUCCEEDED(device->CreateDepthStencilState(&dd, &g_noDepth));
  if (!ok) {
    Log("mirror: could not create its pipeline; desktop mirror off");
    g_failed = true;
    Shutdown();
  }
  return ok;
}

// `c` becomes a copy of `src` (re-created when the size or format changes).
bool CopyOf(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, Copy& c) {
  D3D11_TEXTURE2D_DESC sd;
  src->GetDesc(&sd);
  if (sd.SampleDesc.Count != 1) return false;
  if (c.tex) {
    D3D11_TEXTURE2D_DESC d;
    c.tex->GetDesc(&d);
    if (d.Width != sd.Width || d.Height != sd.Height || d.Format != sd.Format) Release(c);
  }
  if (!c.tex) {
    D3D11_TEXTURE2D_DESC d = sd;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.MipLevels = 1;
    d.ArraySize = 1;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &c.tex)) || FAILED(device->CreateShaderResourceView(c.tex, nullptr, &c.srv))) {
      Release(c);
      return false;
    }
  }
  ctx->CopyResource(c.tex, src);
  return true;
}

// The pipeline state the mirror changes, put back afterwards.
struct Saved {
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  UINT viewports = 1;
  D3D11_VIEWPORT viewport{};
  ID3D11BlendState* blend = nullptr;
  float factor[4]{};
  UINT mask = 0;
  ID3D11DepthStencilState* depth = nullptr;
  UINT stencil = 0;
  ID3D11RasterizerState* raster = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11SamplerState* sampler = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* vscb = nullptr;
  ID3D11InputLayout* layout = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topology{};

  void Save(ID3D11DeviceContext* c) {
    c->OMGetRenderTargets(1, &rtv, &dsv);
    c->RSGetViewports(&viewports, &viewport);
    c->OMGetBlendState(&blend, factor, &mask);
    c->OMGetDepthStencilState(&depth, &stencil);
    c->RSGetState(&raster);
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->PSGetShaderResources(0, 1, &srv);
    c->PSGetSamplers(0, 1, &sampler);
    c->PSGetConstantBuffers(0, 1, &cb);
    c->VSGetConstantBuffers(0, 1, &vscb);
    c->IAGetInputLayout(&layout);
    c->IAGetPrimitiveTopology(&topology);
  }
  void Restore(ID3D11DeviceContext* c) {
    c->OMSetRenderTargets(1, &rtv, dsv);
    if (viewports) c->RSSetViewports(1, &viewport);
    c->OMSetBlendState(blend, factor, mask);
    c->OMSetDepthStencilState(depth, stencil);
    c->RSSetState(raster);
    c->VSSetShader(vs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->PSSetShaderResources(0, 1, &srv);
    c->PSSetSamplers(0, 1, &sampler);
    c->PSSetConstantBuffers(0, 1, &cb);
    c->VSSetConstantBuffers(0, 1, &vscb);
    c->IASetInputLayout(layout);
    c->IASetPrimitiveTopology(topology);
    for (IUnknown* p : std::initializer_list<IUnknown*>{rtv, dsv, blend, depth, raster, vs, ps, srv, sampler, cb, vscb, layout})
      if (p) p->Release();
  }
};

}  // namespace

void KeepEye(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer) {
  g_haveEye = CopyOf(device, ctx, backBuffer, g_eye);
}

void Invalidate() { g_haveEye = false; }

void Compose(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer, const Pose& eye, float tanX,
             float tanY, float fovDeg, const Panel* panels, int panelCount) {
  if (!g_haveEye || tanX <= 0 || tanY <= 0 || !CreatePipeline(device)) return;
  if (panelCount > 0 && !CopyOf(device, ctx, backBuffer, g_hud)) panelCount = 0;
  D3D11_TEXTURE2D_DESC bd;
  backBuffer->GetDesc(&bd);
  ID3D11RenderTargetView* rtv = nullptr;
  if (FAILED(device->CreateRenderTargetView(backBuffer, nullptr, &rtv))) return;

  // Crop: the mirror's field of view (square pixels in the window), never wider than the eye image.
  float t = std::tan(std::clamp(fovDeg, 30.0f, 150.0f) * 0.5f * 3.14159265f / 180.0f);
  const float aspect = float(bd.Height) / float(std::max(1u, bd.Width));
  t = std::min({t, tanX, tanY / aspect});
  Constants k{};
  k.crop[0] = t / tanX;
  k.crop[1] = t * aspect / tanY;
  auto upload = [&] {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    std::memcpy(m.pData, &k, sizeof(k));
    ctx->Unmap(g_cb, 0);
    return true;
  };
  upload();

  Saved saved;
  saved.Save(ctx);
  const D3D11_VIEWPORT vp{0, 0, float(bd.Width), float(bd.Height), 0, 1};
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(g_raster);
  ctx->OMSetDepthStencilState(g_noDepth, 0);
  ctx->IASetInputLayout(nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(g_vs, nullptr, 0);
  ctx->PSSetSamplers(0, 1, &g_sampler);
  ctx->PSSetConstantBuffers(0, 1, &g_cb);
  ctx->VSSetConstantBuffers(0, 1, &g_cb);

  ctx->OMSetBlendState(g_opaque, nullptr, 0xFFFFFFFF);
  ctx->PSSetShader(g_psEye, nullptr, 0);
  ctx->PSSetShaderResources(0, 1, &g_eye.srv);
  ctx->Draw(3, 0);
  if (panelCount > 0) {
    // Each panel's corners seen from the eye (OpenXR: -Z ahead), straight into the window's clip space:
    // x / (-z) / t across, y / (-z) / (t * aspect) up, w = -z.
    const Quat toEye = Conjugate(eye.orientation);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(g_vsPanel, nullptr, 0);
    ctx->OMSetBlendState(g_premul, nullptr, 0xFFFFFFFF);
    ctx->PSSetShader(g_psHud, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &g_hud.srv);
    for (int i = 0; i < panelCount; ++i) {
      const Panel& p = panels[i];
      for (int c = 0; c < 4; ++c) {
        const Vec3 local{(c & 1 ? 0.5f : -0.5f) * p.width, (c & 2 ? -0.5f : 0.5f) * p.height, 0};
        const Vec3 e = Rotate(toEye, p.pose.position + Rotate(p.pose.orientation, local) - eye.position);
        k.corner[c][0] = e.x / t;
        k.corner[c][1] = e.y / (t * aspect);
        k.corner[c][2] = -0.5f * e.z;
        k.corner[c][3] = -e.z;
      }
      std::memcpy(k.uvRect, p.uv, sizeof(k.uvRect));
      if (upload()) ctx->Draw(4, 0);
    }
  }
  ID3D11ShaderResourceView* none = nullptr;
  ctx->PSSetShaderResources(0, 1, &none);
  saved.Restore(ctx);
  rtv->Release();
}

void Shutdown() {
  Release(g_eye);
  Release(g_hud);
  g_haveEye = false;
  SafeRelease(g_vs);
  SafeRelease(g_vsPanel);
  SafeRelease(g_psEye);
  SafeRelease(g_psHud);
  SafeRelease(g_cb);
  SafeRelease(g_sampler);
  SafeRelease(g_opaque);
  SafeRelease(g_premul);
  SafeRelease(g_raster);
  SafeRelease(g_noDepth);
}

}  // namespace gdvr::mirror
