#pragma once
// Head-tracked sound. The game points its listener ("ears") once per frame in
// WorldCamera::SetListenerPosition: along the camera's horizontal facing, up = world up. In stereo the
// mod turns those ears by the head's rotation relative to the game camera, so sounds stay put in the
// world when you look around (an enemy on your left is heard on the right after you turn to the left).

#include <windows.h>

#include "vr_math.h"

namespace gdvr::audio {

bool Install(HMODULE engine);

// Once per stereo frame: the frame the head moves in (game space) and the head's axes in game space.
void SetHeadFrame(Vec3 baseRight, Vec3 baseUp, Vec3 baseForward, Vec3 headRight, Vec3 headUp, Vec3 headForward);

void DrawDebugUI();
int InFlight();

}  // namespace gdvr::audio
