#pragma once

namespace gdvr::overlay {

// Finds IDXGISwapChain::Present/ResizeBuffers via a throwaway device and creates the hooks.
bool Install();

// Asks the render thread to release everything that touches the D3D context or OpenXR
// (XR session, mask, profiler, ImGui) inside its next Present, and waits for it.
// Returns false if no frame was presented within the timeout (nothing is torn down then).
bool RequestTeardown(unsigned timeoutMs);

// Same teardown on the calling thread; only for when the game is not presenting frames.
void TeardownNow();

// Number of threads currently inside one of our swapchain detours.
int InFlight();

}  // namespace gdvr::overlay
