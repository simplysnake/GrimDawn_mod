#pragma once
// Frame profiler: CPU time per engine render stage, GPU time per eye, logged once a second.

#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace gdvr::perf {

// GPU timestamp points within a frame.
enum GpuMark { kEye0Begin, kEye0End, kEye1End, kPresent, kGpuMarkCount };

// Hooks the GraphicsMTRenderer stage functions for CPU timing (hooks created, not enabled).
bool InstallStageHooks(HMODULE engine);

// Records a GPU timestamp (call on the render thread).
void Mark(GpuMark mark);

// CPU time of the eye renders this frame (ms).
void AddEyeCpu(int eye, double ms);

// Called from the Present hook: closes the frame's GPU queries, collects older results and
// writes the once-a-second summary to the log.
void OnPresent(ID3D11Device* device, ID3D11DeviceContext* context, double xrWaitMs);

int InFlight();
void Shutdown();

}  // namespace gdvr::perf
