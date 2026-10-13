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
  int maskDepthPick = 0;        // which screen-sized depth buffer: 0 = first bound (the G-buffer pass, cleared per eye), -1 = last
  bool hud = true;              // show the game's HUD on a floating panel in stereo mode
  float hudDistance = 0.7f;     // metres in front of the recentred position
  float hudWidth = 0.9f;        // metres wide (the panel covers the whole game screen)
  float hudDown = 0.125f;       // metres below eye level
  float hudTiltDeg = 12.0f;     // tilted back so it faces up towards you
  float hudBrightness = 1.0f;   // HUD colour multiplier
  float hudOpacity = 1.0f;      // HUD overall opacity
  // Dark backing (halo) behind HUD elements, stronger and wider as the scene gets brighter, so the
  // HUD stays readable in daylight. The HUD itself keeps its brightness.
  bool hudDayAdapt = true;        // blend the halo from its night to its day values with scene brightness
  float hudHalo = 0.4f;           // night: halo strength (0 = off)
  float hudHaloRadius = 6.0f;     // night: halo size in pixels (HUD image is 2560 px across the panel)
  float hudHaloDay = 0.9f;        // daylight: halo strength
  float hudHaloRadiusDay = 14.0f; // daylight: halo size in pixels
  float hudDayFrom = 0.10f;       // scene brightness that counts as night (measured ~0.08-0.14 at night)
  float hudDayTo = 0.28f;         // ... and as full daylight (measured ~0.24-0.32 in daylight)
  float hudAlphaBoost = 3.0f;   // dark-but-drawn HUD pixels become solid (1 = brightness-only, looks additive)
  bool hudForceDaylight = false; // preview the daylight look at any time of day (not saved)
  bool swapEyeOrder = false;     // render the right eye first (diagnostic)
  bool keepRunningUnfocused = true;  // don't pause when the game window loses focus
  bool bench = false;           // render stereo even while the headset is asleep (for profiling)
};

Settings& GetSettings();
void RequestRecenter();

// Writes the current settings to %LOCALAPPDATA%\gdvr\control.ini (section [vr]).
bool SaveSettings();

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

// Clears the back buffer to transparent black after both eyes are captured, so the game then
// draws only its HUD onto it; the result is shown on the HUD panel.
void PrepareHud();

// Releases the session and everything else (on unload).
void Shutdown();

void DrawDebugUI();

// Saves the next frame's eye images to %LOCALAPPDATA%\gdvr\eye0.bmp / eye1.bmp (also F11).
void RequestEyeDump();

// Time the last xrWaitFrame blocked (ms): how long the headset made the game wait.
double LastWaitMs();

}  // namespace gdvr::xr
