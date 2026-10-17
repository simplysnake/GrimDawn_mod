#pragma once
// World-space fade-to-black around the player, drawn over each eye image.
// Uses the scene's depth buffer (captured while the engine binds it) to rebuild world positions.

#include "vr_math.h"

struct ID3D11DeviceContext;
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
  bool dome = false;           // constellations = devotion map as a dome overhead (its own alpha)
  float domeHorizon = -0.09f;  // the map's rim elevation, radians
};

// Hooks ID3D11DeviceContext::OMSetRenderTargets (once) so the scene depth buffer can be found.
void InstallHook(ID3D11DeviceContext* context);

// Start/stop watching depth-buffer binds (around each eye render).
// width/height: size of the scene depth buffer; pick: -1 = last matching bind, n = n-th distinct one.
void BeginCapture(unsigned width, unsigned height, int pick = -1);
void EndCapture();

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
};
bool FixHudAlpha(IDXGISwapChain* swapChain, const HudLook& look);

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
