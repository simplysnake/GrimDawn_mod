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

// Text drawn straight into the back buffer at screen positions (used for 3D damage numbers, drawn
// into each eye's image before it is captured). Render thread only.
struct WorldText {
  float x, y;         // centre of the text, in back-buffer pixels
  float size;         // text height in pixels
  unsigned color;     // ImGui colour (ABGR)
  const char* text;   // UTF-8
  bool box = false;   // dark box behind the text (item labels); otherwise an outline (numbers)
};
void DrawWorldText(const WorldText* items, int count);

// Number of threads currently inside one of our swapchain detours.
int InFlight();

}  // namespace gdvr::overlay
