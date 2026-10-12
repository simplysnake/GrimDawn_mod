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
};

// Hooks ID3D11DeviceContext::OMSetRenderTargets (once) so the scene depth buffer can be found.
void InstallHook(ID3D11DeviceContext* context);

// Start/stop watching depth-buffer binds (around each eye render).
// width/height: size of the scene depth buffer; pick: -1 = last matching bind, n = n-th distinct one.
void BeginCapture(unsigned width, unsigned height, int pick = -1);
void EndCapture();

// Draws the fade over the back buffer. Returns false if no usable depth buffer was found.
bool Apply(IDXGISwapChain* swapChain, const Params& p);

int InFlight();
void Shutdown();

}  // namespace gdvr::mask
