#pragma once

namespace gdvr::engine {

// Resolves the engine exports and creates (but does not enable) the MinHook hooks.
bool Install();

// Called once per presented frame: publishes this frame's render-call stats to the UI.
void OnFramePresented();

// Draws the ImGui debug window (camera data, render calls, experiments).
void DrawDebugUI();

// Number of threads currently inside one of our engine detours.
int InFlight();

}  // namespace gdvr::engine
