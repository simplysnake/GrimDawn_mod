#pragma once
// World-space fade-to-black around the player, drawn over each eye image.
// Uses the scene's depth buffer (captured while the engine binds it) to rebuild world positions.

#include <functional>

#include "vr_math.h"

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11Texture2D;
struct IDXGISwapChain;

namespace gdvr::mask {

struct Params {
  Vec3 right, up, forward, origin;  // eye camera (screen-right, up, forward) and position
  float tanX = 1, tanY = 1;         // half-angle tangents the eye was rendered with
  float nearPlane = 1, farPlane = 70;
  Vec3 target;                      // fade centre (the player)
  float inner = 45, outer = 62;     // horizontal distance from target: clear inside, black beyond
  float depthStart = 0.85f;         // fraction of the far plane where the far-plane fade starts
  float strength = 1.0f;            // max darkness, 0..1
  bool debug = false;               // show distance rings instead of the fade
  // Sky drawn instead of black (sky.h). Null views = layer missing; no layers = black as before.
  void* skyConstellations = nullptr;  // ID3D11ShaderResourceView*
  void* skyBackdrop = nullptr;        // ID3D11ShaderResourceView*
  Vec3 skyForward, skyRight;          // level frame the sky is laid out in (game camera, horizontal)
  float bandHalfWidth = 1.3f;         // radians
  float bandLow = 0.03f, bandHigh = 0.96f;  // elevations, radians
  float crop[4] = {0, 0, 1, 1};
  float key = 0.1f, soft = 0.15f, brightness = 1.2f, opacity = 1.0f, backdropBrightness = 1.0f;
  int eye = 0;                  // which eye (each keeps its own contact-shadow history)
  float contactStrength = 0;   // contact shadows (darkening where things meet the ground), 0 = off
  float contactSoftness = 1.5f;  // ...their blur (mip level of the half-size occlusion: +1 = twice as wide)
  float contactSize = 1.0f;      // ...how far around a point geometry counts (game units)
  bool dome = false;           // constellations = devotion map as a dome overhead (its own alpha)
  float domeHorizon = -0.09f;  // the map's rim elevation, radians
};

// Hooks ID3D11DeviceContext::OMSetRenderTargets (once) so the scene depth buffer can be found.
void InstallHook(ID3D11DeviceContext* context);

// Start/stop watching depth-buffer binds (around each eye render).
// width/height: size of the scene depth buffer; pick: -1 = last matching bind, n = n-th distinct one.
void BeginCapture(unsigned width, unsigned height, int pick = -1);
void EndCapture();

// ---- Title screen: the character picture standing in the 3D menu scene --------------------------------
// The character is a flat picture in the game's menu layer; it is drawn as a card standing on the scene's
// ground instead (and left out of the HUD panel), so it stays planted when the head moves.
struct EyeCam {
  Vec3 origin, right, up, forward;
  float tanX = 1, tanY = 1, nearPlane = 0.1f;
};
// Where the picture is in the menu layer (fractions u0 v0 u1 v1), from the last frames' title layer.
bool TitleCharacterRect(float rect[4]);
// Asks for the scene depth at a pixel of the eye just rendered; the answer arrives a frame or two later.
void ProbeDepth(float px, float py);
float ProbedDepth();  // raw depth 0..1 (-1: none yet)
// Clears the scene's depth buffers. The menu layer draws the character picture depth-tested against them;
// in VR they hold the last eye's scene, which cut holes in the picture (grass and ground shaped).
void ClearSceneDepth(ID3D11DeviceContext* ctx);
// Draws the picture (`rect` of the last menu layer) on the quad `corners` (BL BR TR TL, game space).
void DrawCutout(IDXGISwapChain* swapChain, const EyeCam& cam, const Vec3 corners[4], const float rect[4]);

// For other passes drawing into the eye image (controller models): runs `draw` with the back buffer's render
// target view (not owned) and size, the engine's pipeline state saved before and restored after (vertex and
// index buffers excepted). BindTargets binds targets without the depth-buffer watch.
bool DrawOverBackBuffer(IDXGISwapChain* swapChain,
                        const std::function<void(ID3D11DeviceContext*, ID3D11RenderTargetView*, unsigned, unsigned)>& draw);
void BindTargets(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv);

// Draws the fade over the back buffer. Returns false if no usable depth buffer was found.
bool Apply(IDXGISwapChain* swapChain, const Params& p);

// HUD pass: makes every visible HUD pixel opaque enough to show (alpha >= brightness), since some
// HUD elements are drawn without writing alpha. The HUD was drawn over transparent black, so its
// colours are already premultiplied. Works in place on the back buffer.
struct HudLook {
  float brightness = 1.0f;  // colour multiplier
  float opacity = 1.0f;     // overall opacity
  float halo = 0.0f;        // strength of the dark halo behind HUD elements (0 = off)
  float haloRadius = 3.0f;  // halo size in pixels
  float alphaBoost = 1.0f;  // opacity from brightness x this (makes dark HUD parts solid)
  // A top-right block of the HUD (fractions of the image) drawn shifted left by sideShift (fraction of width).
  float sideWidth = 0, sideHeight = 0, sideShift = 0;
  bool sideOutline = false;
  bool keyBlack = false;  // opacity from brightness only (ignores the game's alpha): black is see-through
  float cut[4] = {};      // title screen: rect (fractions u0 v0 u1 v1) left out of the panel (the character)
};
bool FixHudAlpha(IDXGISwapChain* swapChain, const HudLook& look);

// Player window on the hand, after FixHudAlpha. `remember` (no menu open): keep the HUD as it is as the
// reference (the one from half a second before is used). Otherwise, with the window's rectangle `base` (and
// the tooltip's, `tip`, may be null; fractions u0 v0 u1 v1): returns an image (owned by the mask, valid until
// the next call) holding all of the window and what in `tip` differs from the reference, transparent elsewhere,
// and changes the back buffer to show the reference there instead. Null if not split (remembering, no reference
// yet, no `base`). `hide` (may be null; also without a window on the hand): left out of the back buffer except
// what differs from the reference (the bottom bar while a menu is open, but not a menu reaching over it).
ID3D11Texture2D* SplitHud(IDXGISwapChain* swapChain, bool remember, const float* base, const float* tip,
                          const float* hide = nullptr);

// Measures the average brightness of the current back buffer (an eye image) on the GPU; the
// result arrives a few frames later. Call once per frame after an eye is rendered.
void MeasureBrightness(IDXGISwapChain* swapChain);
// Latest average brightness of the scene, 0..1 (gamma space), or -1 if not measured yet.
float SceneBrightness();

// Average coverage of the finished HUD image (0..1), or -1 before the first reading. Menus raise it.
float HudCoverage();

// HUD opacity around an image position (fractions 0..1) from a 160x90 map read back every few
// frames; -1 if not measured yet.
float HudAlphaAt(float u, float v);
// Bounding box (fractions) of what the HUD shows inside `region` (u0 v0 u1 v1), leaving out circles of
// `skipRadius` (screen heights) around the `skip` points (u, v); false if nothing.
// Share (0..1) of the HUD image where something is drawn, leaving out the `skipRects` (u0 v0 u1 v1) and circles
// of `skipRadius` (screen heights) around the `skip` points; -1 if not measured yet.
float HudContentCoverage(const float (*skipRects)[4], int rectCount, const float (*skip)[2], int skipCount,
                         float skipRadius);
bool HudContentBounds(const float region[4], const float (*skip)[2], int skipCount, float skipRadius, float out[4]);
// Player menus on the hand: the bounding box (fractions) of what the HUD shows now that it didn't show
// before the menu opened. `remember` (no menu open): keep the current image as that reference (the one
// from half a second ago is used, so a menu fading in isn't in it). Only columns where at least
// `minColumn` (share of the rows) is new count, so small things (buff icons, the cursor) don't widen it;
// cells inside the `skipRects` never count. With an `anchor` (u0 v0 u1 v1, may be null): false unless a run
// of such columns overlaps it (the window is still there); then the bounds are everything new connected to
// the anchor or to the `cursor` (u v, may be null): a tooltip over or beside the window, however short,
// followed down into a skip rect below it where it covers what was there before. False if nothing (or while
// remembering). `tip` (may be null): with an anchor, the box of what of that lies outside the anchor (the
// tooltip), all zero if nothing does.
bool HudNewContentBounds(bool remember, const float (*skipRects)[4], int rectCount, float minColumn,
                         const float* anchor, const float* cursor, float out[4], float* tip = nullptr);
// Share (0..1) of a HUD region (fractions of the image) where something is drawn, or -1 if not measured yet.
float HudRegionCoverage(float u0, float v0, float u1, float v1);

// ---- Per-eye depth history ---------------------------------------------------------------
// The engine's geometry pass samples a copy of the PREVIOUS render's depth (soft blending of
// grass/rocks/characters into the ground); the copy is refreshed only after the geometry pass.
// In stereo the previous render is the other eye, which gives misaligned dark edges. These keep
// one history per eye: restore it before the eye renders, save it afterwards.
void OnCopyResource(void* dst, void* src);  // called for copies made during an eye render
void RestoreEyeHistory(int eye);
void SaveEyeHistory(int eye);
// Mode: 0 = engine default (other eye's depth), 1 = per-eye history (aligned but a frame late),
// 2 = cleared to far depth before each eye (soft blending switched off; the default).
void SetDepthHistoryMode(int mode);
int DepthHistoryMode();

int InFlight();
void Shutdown();

}  // namespace gdvr::mask
