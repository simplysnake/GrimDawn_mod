#pragma once
// OpenXR (SteamVR) output: session, frame loop, per-eye capture.

#include "mask.h"
#include "vr_math.h"

struct IDXGISwapChain;

namespace gdvr::xr {

enum class Mode : int { Off = 0, Screen = 1, Stereo = 2 };

struct Settings {
  Mode mode = Mode::Stereo;
  float worldScale = 15.0f;     // game units per real metre (bigger = you are a giant over a miniature)
  float fovDeg = 0.0f;          // vertical FOV for each eye; 0 = fit the headset's lenses automatically
  bool levelHorizon = true;     // map real "straight ahead" to the game's horizon, not its tilted camera
  bool recomputeFrustum = true; // cull with the eye camera's own frustum (fixes holes when looking around)
  float screenDistance = 2.0f;  // Screen mode: metres in front of you
  float screenWidth = 3.0f;     // Screen mode: metres wide
  float cameraPull = 6.0f;       // game units the viewpoint moves towards the character (closer zoom range)
  bool contactShadowsOn = true;   // contact shadows on/off (keeps the strength below)
  float contactShadows = 0.6f;   // darkening where grass, rocks and characters meet the ground (0 = off)
  float contactSoftness = 1.5f;  // ...how soft (blur level)
  float contactSize = 1.0f;      // ...how far from a contact the darkening reaches (game units)
  bool titleCutout = true;       // title screen: the character picture stands in the 3D scene
  bool mirror = true;            // the game window shows the left eye with the HUD (for recording/streaming)
  float mirrorFovDeg = 90.0f;    // ...cropped to this horizontal field of view
  float farPlane = 70.0f;       // far plane for the eye cameras (game: 120); the biggest performance lever
  bool maskEnabled = true;       // fade to black around the player
  float maskInner = 45.0f;      // horizontal distance from the player where the fade starts (game units)
  float maskOuter = 62.0f;      // ... and where it is fully black
  float maskDepthStart = 0.85f; // fraction of the far plane where the far-plane fade starts
  float maskStrength = 1.0f;    // 0..1
  bool maskDebug = false;
  // Sky beyond the play area (sky.h): backdrop panorama + captured devotion constellations.
  bool credit = true;
  char skySkip[256] = "";        // UI texture kinds left out of the capture ("WxH fN, ..."), from the UI trace             // "created by artalva.com" on the HUD panel when VR starts
  bool sky = true;
  int skySource = 0;               // constellations: 0 = the devotion map from the game files (dome), 1 = screen capture (band)
  float skyDomeHorizonDeg = -5.0f;
  float skyIncomplete = 0.45f;    // brightness of constellations you haven't completed
  float skyGlow = 0.5f;           // glow of completed constellations and allocated stars // the map's rim sits at this elevation (its centre straight overhead)
  float skyBandWidthDeg = 150.0f;  // how wide the constellation band is around you
  float skyBandLowDeg = 2.0f;      // band from this elevation...
  float skyBandHighDeg = 55.0f;    // ...to this one
  float skyBandYawDeg = 0.0f;      // turn the band (0 = in front of the game camera)
  float skyCrop[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // part of the capture used (left, top, right, bottom)
  float skyKey = 0.10f;            // capture brightness below this is background (transparent)
  float skySoft = 0.15f;           // ...fading in over this much
  float skyBrightness = 1.2f;
  float skyOpacity = 1.0f;
  float skyBackdropBrightness = 1.0f;       // distance rings (green inside, red fading, blue black)
  int maskDepthPick = 0;        // which screen-sized depth buffer: 0 = first bound (the G-buffer pass, cleared per eye), -1 = last
  bool hud = true;              // show the game's HUD on a floating panel in stereo mode
  float hudDistance = 0.72f;    // metres in front of the recentred position
  float hudWidth = 2.15f;       // metres wide (the panel covers the whole game screen)
  float hudDown = 0.19f;        // metres below eye level
  float hudTiltDeg = 32.0f;     // tilted back so it faces up towards you
  // Minimap + quest tracker block (top-right corner of the HUD image), moved left across the panel.
  float hudSideShift = 0.0f;    // how far left, as a fraction of the HUD width (0 = where the game draws it)
  float hudSideWidth = 0.22f;   // the block's size as fractions of the HUD image, from the top-right corner
  float hudSideHeight = 0.55f;
  float hudMenuCoverage = 0.25f; // HUD coverage above this = a menu is open: the block goes back to its place
  bool hudSideOutline = false;
  bool pointer = true;           // controller laser pointer
  int pointerHand = 1;           // 0 left, 1 right
  bool pureController = false;   // VR controllers as a gamepad (game's gamepad mode, no laser)
  bool itemLabels3d = true;      // item names drawn by the mod in 3D at the item
  bool damageNumbers3d = true;   // damage numbers drawn by the mod in 3D (true depth) instead of the flat UI
  bool labelsInWorld = true;     // health bars, names, damage numbers drawn where you see their monster/item  // outline the block (red: where the game draws it, green: where it is shown; not saved)
  float hudBrightness = 1.5f;   // HUD colour multiplier
  float hudOpacity = 1.0f;      // HUD overall opacity
  // Dark backing (halo) behind HUD elements, stronger and wider as the scene gets brighter, so the
  // HUD stays readable in daylight. The HUD itself keeps its brightness.
  bool hudDayAdapt = true;        // blend the halo from its night to its day values with scene brightness
  float hudHalo = 0.83f;          // night: halo strength (0 = off)
  float hudHaloRadius = 20.0f;    // night: halo size in pixels (HUD image is 2560 px across the panel)
  float hudHaloDay = 1.0f;        // daylight: halo strength
  float hudHaloRadiusDay = 14.0f; // daylight: halo size in pixels
  float hudDayFrom = 0.10f;       // scene brightness that counts as night (measured ~0.08-0.14 at night)
  float hudDayTo = 0.28f;         // ... and as full daylight (measured ~0.24-0.32 in daylight)
  float hudAlphaBoost = 8.0f;  // dark-but-drawn HUD pixels become solid (1 = brightness-only, looks additive)
  bool hudForceDaylight = false; // preview the daylight look at any time of day (not saved)
  bool swapEyeOrder = false;     // render the right eye first (diagnostic)
  bool keepRunningUnfocused = true;  // don't pause when the game window loses focus
  bool bench = false;           // render stereo even while the headset is asleep (for profiling)
};

Settings& GetSettings();

// The game's swap chain (null before the first frame).
IDXGISwapChain* GameSwapChain();

// Where the line from the centre of your eyes to a point (recentred space, metres) crosses the HUD
// panel, in HUD image fractions (0..1 across, 0..1 down). False if the panel is behind you or edge-on.
bool HudPanelPoint(Vec3 pointRel, float& u, float& v);

// Controller aim pose (hand 0 left, 1 right) relative to the recentred origin; false if not tracked.
bool HandAim(int hand, Pose& pose);

// Where a ray (recentred space, unit direction) hits the HUD panel: image fractions and distance (m).
bool HudPanelRay(Vec3 originRel, Vec3 dirRel, float& u, float& v, float& distance);

// Whether the HUD shows something at this image position that should catch the pointer (`sticky`:
// the pointer is already on the HUD, so fainter parts count too). The whole panel while a menu is open.
bool HudSolidAt(float u, float v, bool sticky = false);

// A game menu (inventory, skills, options...) is open, judged from HUD coverage.
bool HudMenuOpen();
void RequestRecenter();

// Writes the current settings to %LOCALAPPDATA%\gdvr\control.ini (section [vr]).
bool SaveSettings();

// Called from the Present hook, before the game's Present: submits the previous frame and
// begins the next one (this is where OpenXR paces the game).
void OnPresent(IDXGISwapChain* swapChain);

// True when this frame should be rendered once per eye.
bool StereoFrameActive();

// Whether the world being rendered has a character in it (false on the title screen). From the renderer.
void SetInWorld(bool inWorld);

// Title screen: the character picture's rect in the menu layer (fractions u0 v0 u1 v1), if found.
bool TitleCutout(float rect[4]);

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

void DrawDebugUI();      // settings: HUD placement/look, world, save
void DrawDiagnostics();  // session details

// Saves the next frame's eye images to %LOCALAPPDATA%\gdvr\eye0.bmp / eye1.bmp (also F11).
void RequestEyeDump();

// Time the last xrWaitFrame blocked (ms): how long the headset made the game wait.
double LastWaitMs();

}  // namespace gdvr::xr
