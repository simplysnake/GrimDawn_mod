#pragma once
// Pointer in VR: a ray from a VR controller (the laser) hits the HUD panel or the ground around the
// character; the hit is drawn in both eyes and becomes the game's mouse: its screen position is
// returned from GetCursorPos to the game's input module, and the left trigger is the left button
// (added to the mouse state the game reads through DirectInput).

#include <windows.h>

#include "numbers.h"
#include "vr_math.h"

namespace gdvr::pointer {

bool Install();

// The game's own (flat-screen) camera: a world point's position on the game's screen is the mouse
// position the game needs for hover, picking and click-to-move.
struct GameView {
  Vec3 origin, right, up, forward;
  float tanX = 1, tanY = 1;
  float width = 0, height = 0;  // the game's back buffer / window client size
};

// Once per stereo frame, before the eyes: the game-space mapping of the eye cameras (as in
// labels::SetStereoFrame), the character's position and the game's own camera.
void Update(Vec3 origin, Vec3 right, Vec3 up, Vec3 fwd, float scale, Vec3 target, bool haveTarget,
            const GameView& game);

// Draws the laser and the cursor ring into this eye's image.
void DrawEye(const numbers::EyeView& view);

// True while stereo frames keep updating the pointer.
bool Live();

void DrawDebugUI();
int InFlight();

}  // namespace gdvr::pointer
