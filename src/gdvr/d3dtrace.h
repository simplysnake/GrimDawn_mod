#pragma once
// Diagnostic: records the D3D11 work (render-target switches, clears, draws) issued during each
// eye's render for one frame, and writes both sequences side by side to
// %LOCALAPPDATA%\gdvr\trace_eye0.txt / trace_eye1.txt, to find passes missing from one eye.

struct ID3D11DeviceContext;

namespace gdvr::d3dtrace {

// Hooks the draw/clear calls on the context's vtable (once).
void InstallHooks(ID3D11DeviceContext* context);

// Arms recording for the next stereo frame.
void Arm();

// Called around each eye's render: eye 0/1 begin recording into that eye's list, -1 stops.
void SetEye(int eye);

// Called by the OMSetRenderTargets hook (mask.cpp) while recording.
void OnSetRenderTargets(unsigned numViews, void* const* rtvs, void* dsv);

// Pass switcher: drop all draws in render-target segment n of each eye render (-1 = off).
void SetSkipSegment(int segment);
int SkipSegment();

int InFlight();

}  // namespace gdvr::d3dtrace
