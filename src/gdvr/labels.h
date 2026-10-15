#pragma once
// World-anchored UI (health bars, damage numbers, item names): finding where the game projects them.
// Step 1 is diagnostic: count who calls the engine's world->screen projection, and when.

#include <windows.h>

#include "numbers.h"
#include "vr_math.h"

namespace gdvr::labels {

bool Install(HMODULE engine);

// Marks the world render on this thread (projections inside it are the renderer's, not UI labels).
struct WorldRenderScope {
  WorldRenderScope();
  ~WorldRenderScope();
};

// The stereo view of this frame: game point = origin + (right*x + up*y + fwd*z) * scale, where
// (x, y, -z) is the point in recentred headset space (metres). Labels are placed with it.
void SetStereoFrame(Vec3 origin, Vec3 right, Vec3 up, Vec3 fwd, float scale);

// Once per presented frame (render thread).
void OnFramePresented();

// Item names (on the ground, chests...) drawn into this eye at their 3D anchors.
void DrawEye(const numbers::EyeView& view);

void DrawDebugUI();
int InFlight();

}  // namespace gdvr::labels
