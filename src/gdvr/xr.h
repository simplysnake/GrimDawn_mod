#pragma once
// OpenXR (SteamVR) output: session, frame loop, per-eye capture.

#include "mask.h"
#include "vr_math.h"

struct IDXGISwapChain;

namespace gdvr::xr {

enum class Mode : int { Off = 0, Screen = 1, Stereo = 2 };

struct Settings {
  Mode mode = Mode::Screen;
  float worldScale = 15.0f;     // game units per real metre (bigger = you are a giant over a miniature)
  float fovDeg = 0.0f;          // vertical FOV for each eye; 0 = fit the headset's lenses automatically
  bool levelHorizon = true;     // map real "straight ahead" to the game's horizon, not its tilted camera
  bool recomputeFrustum = true; // cull with the eye camera's own frustum (fixes holes when looking around)
  float screenDistance = 2.0f;  // Screen mode: metres in front of you
  float screenWidth = 3.0f;     // Screen mode: metres wide
  float farPlane = 70.0f;       // far plane for the eye cameras (game: 120); the biggest performance lever
  bool maskEnabled = true;       // fade to black around the player
  float maskInner = 45.0f;      // horizontal distance from the player where the fade starts (game units)
  float maskOuter = 62.0f;      // ... and where it is fully black
  float maskDepthStart = 0.85f; // fraction of the far plane where the far-plane fade starts
  float maskStrength = 1.0f;    // 0..1
  bool maskDebug = false;       // distance rings (green inside, red fading, blue black)
  int maskDepthPick = -1;       // which screen-sized depth buffer to use: -1 = last bound, 0/1 = first/second
  bool bench = false;           // render stereo even while the headset is asleep (for profiling)
};

Settings& GetSettings();
void RequestRecenter();

// Called from the Present hook, before the game's Present: submits the previous frame and
// begins the next one (this is where OpenXR paces the game).
void OnPresent(IDXGISwapChain* swapChain);

// True when this frame should be rendered once per eye.
bool StereoFrameActive();

// Eye pose for this frame, relative to the recentred origin (OpenXR axes: +Y up, -Z forward, metres).
Pose EyePose(int eye);

// tan of the half-angles a symmetric frustum needs to cover this eye's lens (from the runtime).
void EyeTanFov(int eye, float& tanHalfX, float& tanHalfY);

// Copies the current back buffer into the eye's swapchain image. tanHalfX/Y describe the
// symmetric frustum the game actually rendered with.
void CaptureEye(int eye, float tanHalfX, float tanHalfY);

// Draws the fade mask over the current back buffer (before CaptureEye).
void ApplyMask(const mask::Params& p);

// Releases the session and everything else (on unload).
void Shutdown();

void DrawDebugUI();

// Time the last xrWaitFrame blocked (ms): how long the headset made the game wait.
double LastWaitMs();

}  // namespace gdvr::xr
