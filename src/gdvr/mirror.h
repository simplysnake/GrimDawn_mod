#pragma once
// Desktop mirror: the game window shows what you see in the headset (one eye, cropped to a normal field
// of view, with the HUD panels where they float in the headset), so recording and streaming tools can
// capture the game window.

#include "vr_math.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace gdvr::mirror {

// A HUD panel as the headset shows it: a rectangle of the HUD image on a quad in LOCAL space.
struct Panel {
  Pose pose;          // centre of the quad, facing +Z
  float width = 0, height = 0;
  float uv[4] = {};   // u0, v0, u1, v1 of the HUD image
};

// Keeps a copy of the eye image now in the back buffer (call after that eye is rendered).
void KeepEye(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer);

// Draws the kept eye into the back buffer, then the HUD that is in it now on the given panels, seen from
// `eye` (the kept eye's pose in LOCAL space). tanX/tanY: the eye image's half field of view (tangents);
// fovDeg: the mirror's horizontal field of view.
void Compose(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer, const Pose& eye, float tanX,
             float tanY, float fovDeg, const Panel* panels, int panelCount);

// Forgets the eye copy (e.g. a frame without stereo, so an old picture isn't shown).
void Invalidate();

void Shutdown();

}  // namespace gdvr::mirror
