#pragma once
// The devotion constellation map, built from the game's own files (never shipped with the mod):
// the database (.arz: constellation layout, stars, nebulas) and the UI archives (.arc: their art)
// of the base game and the installed expansions. Loaded on a background thread, then drawn once
// into one texture on the GPU, used by the sky (sky.h) as a dome overhead.

#include <string>

struct IDXGISwapChain;

namespace gdvr::devotion {

// Starts loading (once). Cheap to call repeatedly.
void Start();

// The composed map (ID3D11ShaderResourceView*), or null while loading / if it failed. Render thread:
// composes the map the first time the loaded data is ready.
void* MapSrv(IDXGISwapChain* swapChain);

// Brightness of constellations not yet completed (0..1) and glow strength of completed ones.
void SetLook(float incompleteBrightness, float glow);

std::string Status();
void Shutdown();

}  // namespace gdvr::devotion
