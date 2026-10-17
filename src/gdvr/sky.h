#pragma once
// The sky beyond the play area: a backdrop panorama (equirectangular, from the user) with the
// game's own devotion constellations on top, captured from the Devotion screen (bright parts kept,
// its dark background keyed out). Both are drawn by the fade-mask shader in the void around the
// play area, fixed in the world.
//
// Files (dev builds: <repo>/sky, otherwise %LOCALAPPDATA%\gdvr\sky):
//   backdrop.png / backdrop.jpg   the backing sky (2:1 panorama)
//   constellations.png            written by "Capture constellations" (F8)

struct IDXGISwapChain;

namespace gdvr::sky {

// Shader inputs for the mask pass (null views = layer missing).
struct Layers {
  void* constellations = nullptr;  // ID3D11ShaderResourceView*
  void* backdrop = nullptr;        // ID3D11ShaderResourceView*
  bool dome = false;               // constellations are the devotion map (dome, own alpha), not a capture (band)
};
// Loads the images on first use (render thread).
Layers Get(IDXGISwapChain* swapChain);

// F8: capture the Devotion screen (the cursor is parked off-screen first, so no hover text).
void RequestCapture();
void RequestReload();

// Called each stereo frame with the HUD image (UI only) in the back buffer, before the HUD pass.
void OnHudFrame(IDXGISwapChain* swapChain);

void DrawDebugUI();
void Shutdown();

}  // namespace gdvr::sky
