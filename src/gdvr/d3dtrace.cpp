#include "d3dtrace.h"

#include <windows.h>
#include <d3d11.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "mask.h"

namespace gdvr::d3dtrace {

namespace {

// ID3D11DeviceContext vtable slots (OMSetRenderTargets = 33 is hooked by mask.cpp).
constexpr int kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21,
              kDrawIndexedInstancedIndirect = 39, kDrawInstancedIndirect = 40, kDispatch = 41,
              kClearRenderTargetView = 50, kClearDepthStencilView = 53, kExecuteCommandList = 58,
              kCopySubresourceRegion = 46, kCopyResource = 47, kPSSetShaderResources = 8,
              kVSSetShaderResources = 25;

std::atomic<int> g_inFlight{0};
std::atomic<bool> g_armed{false};
std::atomic<int> g_eye{-1};       // eye being recorded, -1 = none
std::mutex g_mutex;
std::vector<std::string> g_lines[2];
int g_draws = 0;                  // draws since the last line was flushed
std::string g_target = "?";       // current render-target description
bool g_hooked = false;

// Pass switcher: render-target segments inside the eye render are numbered from 0 (one per
// OMSetRenderTargets); draws in segment g_skipSegment are dropped in both eyes (-1 = off).
std::atomic<int> g_segment{-1};
std::atomic<int> g_skipSegment{-1};
std::atomic<int> g_renderEye{-1};  // eye being rendered (independent of recording)

bool SkipDraw() { return g_renderEye >= 0 && g_skipSegment >= 0 && g_segment == g_skipSegment; }

struct Guard {
  Guard() { ++g_inFlight; }
  ~Guard() { --g_inFlight; }
};

std::string Describe(ID3D11View* view) {
  if (!view) return "none";
  ID3D11Resource* r = nullptr;
  view->GetResource(&r);
  ID3D11Texture2D* t = nullptr;
  std::string s = "?";
  if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t)))) {
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    char buf[96];
    snprintf(buf, sizeof(buf), "%ux%u f%d %p", d.Width, d.Height, int(d.Format), static_cast<void*>(t));
    s = buf;
    t->Release();
  }
  if (r) r->Release();
  return s;
}

void FlushDraws() {
  if (g_draws) g_lines[g_eye].push_back("    draws " + std::to_string(g_draws));
  g_draws = 0;
}

void Record(const std::string& line) {
  std::lock_guard lock(g_mutex);
  if (g_eye < 0) return;
  FlushDraws();
  g_lines[g_eye].push_back(line);
}

std::atomic<unsigned long long> g_totalDraws{0};

void CountDraw() {
  ++g_totalDraws;
  if (g_eye < 0) return;
  std::lock_guard lock(g_mutex);
  if (g_eye >= 0) ++g_draws;
}

// Original functions
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using IndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using CopySubFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*,
                                           UINT, const D3D11_BOX*);

DrawIndexedFn o_DrawIndexed;
DrawFn o_Draw;
DrawIndexedInstancedFn o_DrawIndexedInstanced;
DrawInstancedFn o_DrawInstanced;
IndirectFn o_DrawIndexedInstancedIndirect, o_DrawInstancedIndirect;
DispatchFn o_Dispatch;
ClearRtvFn o_ClearRtv;
ClearDsvFn o_ClearDsv;
ExecuteFn o_Execute;
CopyResourceFn o_CopyResource;
CopySubFn o_CopySub;
using SetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
SetSrvFn o_PSSetSrv, o_VSSetSrv;
std::vector<std::string> g_srvSeen[2];  // distinct screen-sized SRVs bound per eye (with segment)

void NoteSrvs(const char* stage, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
  if (g_eye < 0 || !v) return;
  for (UINT i = 0; i < n; ++i) {
    if (!v[i]) continue;
    ID3D11Resource* r = nullptr;
    v[i]->GetResource(&r);
    ID3D11Texture2D* tex = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&tex)))) {
      D3D11_TEXTURE2D_DESC d;
      tex->GetDesc(&d);
      if (d.Width >= 1000 && d.Height >= 600 && d.Width != d.Height) {
        char buf[160];
        snprintf(buf, sizeof(buf), "  %s srv slot %u: %ux%u f%d %p (seg %d)", stage, start + i, d.Width, d.Height,
                 int(d.Format), static_cast<void*>(tex), g_segment.load());
        std::lock_guard lock(g_mutex);
        auto& seen = g_srvSeen[g_eye];
        if (std::find(seen.begin(), seen.end(), std::string(buf)) == seen.end()) {
          seen.push_back(buf);
          if (g_eye >= 0) g_lines[g_eye].push_back(buf);
        }
      }
      tex->Release();
    }
    if (r) r->Release();
  }
}
void STDMETHODCALLTYPE H_PSSetSrv(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
  Guard g;
  NoteSrvs("ps", s, n, v);
  o_PSSetSrv(c, s, n, v);
}
void STDMETHODCALLTYPE H_VSSetSrv(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
  Guard g;
  NoteSrvs("vs", s, n, v);
  o_VSSetSrv(c, s, n, v);
}

void STDMETHODCALLTYPE H_DrawIndexed(ID3D11DeviceContext* c, UINT a, UINT b, INT d) {
  Guard g; CountDraw(); if (SkipDraw()) return; o_DrawIndexed(c, a, b, d);
}
void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, UINT a, UINT b) {
  Guard g; CountDraw(); if (SkipDraw()) return; o_Draw(c, a, b);
}
void STDMETHODCALLTYPE H_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) {
  Guard g; CountDraw(); if (SkipDraw()) return; o_DrawIndexedInstanced(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE H_DrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
  Guard g; CountDraw(); if (SkipDraw()) return; o_DrawInstanced(c, a, b, d, e);
}
void STDMETHODCALLTYPE H_DrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
  Guard g; CountDraw(); o_DrawIndexedInstancedIndirect(c, b, o);
}
void STDMETHODCALLTYPE H_DrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
  Guard g; CountDraw(); o_DrawInstancedIndirect(c, b, o);
}
void STDMETHODCALLTYPE H_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
  Guard g;
  if (g_eye >= 0) Record("  dispatch " + std::to_string(x) + "x" + std::to_string(y) + "x" + std::to_string(z));
  o_Dispatch(c, x, y, z);
}
void STDMETHODCALLTYPE H_ClearRtv(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
  Guard g;
  if (g_eye >= 0) Record("  clear rt " + Describe(v));
  o_ClearRtv(c, v, col);
}
void STDMETHODCALLTYPE H_ClearDsv(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s) {
  Guard g;
  if (g_eye >= 0) Record("  clear ds " + Describe(v) + " flags " + std::to_string(f));
  o_ClearDsv(c, v, f, d, s);
}
void STDMETHODCALLTYPE H_Execute(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL r) {
  Guard g;
  if (g_eye >= 0) Record("  execute command list");
  o_Execute(c, l, r);
}

std::string DescribeResource(ID3D11Resource* r) {
  ID3D11Texture2D* t = nullptr;
  std::string s = "buffer/other";
  if (r && SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t)))) {
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    char buf[96];
    snprintf(buf, sizeof(buf), "%ux%u f%d %p", d.Width, d.Height, int(d.Format), static_cast<void*>(t));
    s = buf;
    t->Release();
  }
  return s;
}
void STDMETHODCALLTYPE H_CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
  Guard g;
  if (g_renderEye >= 0) mask::OnCopyResource(dst, src);
  if (g_eye >= 0) Record("  copy " + DescribeResource(src) + " -> " + DescribeResource(dst));
  o_CopyResource(c, dst, src);
}
void STDMETHODCALLTYPE H_CopySub(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z,
                                 ID3D11Resource* src, UINT ss, const D3D11_BOX* box) {
  Guard g;
  if (g_eye >= 0) Record("  copysub " + DescribeResource(src) + " -> " + DescribeResource(dst));
  o_CopySub(c, dst, ds, x, y, z, src, ss, box);
}

template <typename T>
void Hook(void** vtable, int slot, void* detour, T& original) {
  if (MH_CreateHook(vtable[slot], detour, reinterpret_cast<void**>(&original)) == MH_OK) MH_EnableHook(vtable[slot]);
}

void WriteFiles() {
  wchar_t base[MAX_PATH];
  GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  for (int eye = 0; eye < 2; ++eye) {
    std::wstring path = std::wstring(base) + L"\\gdvr\\trace_eye" + std::to_wstring(eye) + L".txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") == 0 && f) {
      for (const std::string& l : g_lines[eye]) fprintf(f, "%s\n", l.c_str());
      fclose(f);
    }
  }
  Log("trace: eye0 %zu lines, eye1 %zu lines written", g_lines[0].size(), g_lines[1].size());
}

}  // namespace

void InstallHooks(ID3D11DeviceContext* context) {
  if (g_hooked) return;
  g_hooked = true;
  void** vt = *reinterpret_cast<void***>(context);
  Hook(vt, kDrawIndexed, reinterpret_cast<void*>(&H_DrawIndexed), o_DrawIndexed);
  Hook(vt, kDraw, reinterpret_cast<void*>(&H_Draw), o_Draw);
  Hook(vt, kDrawIndexedInstanced, reinterpret_cast<void*>(&H_DrawIndexedInstanced), o_DrawIndexedInstanced);
  Hook(vt, kDrawInstanced, reinterpret_cast<void*>(&H_DrawInstanced), o_DrawInstanced);
  Hook(vt, kDrawIndexedInstancedIndirect, reinterpret_cast<void*>(&H_DrawIndexedInstancedIndirect),
       o_DrawIndexedInstancedIndirect);
  Hook(vt, kDrawInstancedIndirect, reinterpret_cast<void*>(&H_DrawInstancedIndirect), o_DrawInstancedIndirect);
  Hook(vt, kDispatch, reinterpret_cast<void*>(&H_Dispatch), o_Dispatch);
  Hook(vt, kClearRenderTargetView, reinterpret_cast<void*>(&H_ClearRtv), o_ClearRtv);
  Hook(vt, kClearDepthStencilView, reinterpret_cast<void*>(&H_ClearDsv), o_ClearDsv);
  Hook(vt, kExecuteCommandList, reinterpret_cast<void*>(&H_Execute), o_Execute);
  Hook(vt, kCopyResource, reinterpret_cast<void*>(&H_CopyResource), o_CopyResource);
  Hook(vt, kCopySubresourceRegion, reinterpret_cast<void*>(&H_CopySub), o_CopySub);
  Hook(vt, kPSSetShaderResources, reinterpret_cast<void*>(&H_PSSetSrv), o_PSSetSrv);
  Hook(vt, kVSSetShaderResources, reinterpret_cast<void*>(&H_VSSetSrv), o_VSSetSrv);
  Log("trace: draw/clear hooks installed");
}

void Arm() { g_armed = true; }

void SetEye(int eye) {
  g_renderEye = eye;
  g_segment = eye >= 0 ? -1 : -1;
  std::lock_guard lock(g_mutex);
  if (eye >= 0) {
    if (!g_armed) return;
    if (eye == 0) {
      g_lines[0].clear();
      g_lines[1].clear();
      g_srvSeen[0].clear();
      g_srvSeen[1].clear();
    }
    g_draws = 0;
    g_eye = eye;
  } else if (g_eye >= 0) {
    FlushDraws();
    const int finished = g_eye;
    g_eye = -1;
    if (finished == 1) {
      g_armed = false;
      WriteFiles();
    }
  }
}

void OnSetRenderTargets(unsigned numViews, void* const* rtvs, void* dsv) {
  if (g_renderEye >= 0) ++g_segment;
  if (g_eye < 0) return;
  std::string s = "[seg " + std::to_string(g_segment.load()) + "] set rt";
  for (unsigned i = 0; i < numViews && i < 4; ++i)
    s += " [" + Describe(rtvs ? static_cast<ID3D11View*>(rtvs[i]) : nullptr) + "]";
  s += " ds [" + Describe(static_cast<ID3D11View*>(dsv)) + "]";
  Record(s);
}

int InFlight() { return g_inFlight.load(); }

unsigned long long TotalDraws() { return g_totalDraws.load(); }

void SetSkipSegment(int segment) { g_skipSegment = segment; }
int SkipSegment() { return g_skipSegment.load(); }

}  // namespace gdvr::d3dtrace
