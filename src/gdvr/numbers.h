#pragma once
// 3D damage numbers. The game announces each one with EventManager::Send (type 0x1B: position,
// text, crit style, size); in stereo the mod keeps the event from the game's flat UI and draws the
// number itself into each eye at its world position, so it has real depth.

#include <windows.h>

#include "vr_math.h"

namespace gdvr::numbers {

bool Install(HMODULE engine);

// One eye's camera, as rendered: game-space position and axes, half-angle tangents, image size.
struct EyeView {
  Vec3 origin, right, up, forward;
  float tanX, tanY;
  float nearPlane;
  float width, height;
};

// Draws the live numbers into the back buffer for this eye (before the eye image is captured).
void DrawEye(const EyeView& view);

// Called once per stereo frame (the numbers are only taken from the game while stereo is on).
void OnStereoFrame();

void DrawDebugUI();
int InFlight();

}  // namespace gdvr::numbers
