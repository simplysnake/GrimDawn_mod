#pragma once

namespace gdvr::overlay {

// Finds IDXGISwapChain::Present/ResizeBuffers via a throwaway device and creates the hooks.
bool Install();

// Restores the game's window procedure and releases ImGui/D3D objects. Call after hooks are disabled.
void Shutdown();

// Number of threads currently inside one of our swapchain detours.
int InFlight();

}  // namespace gdvr::overlay
