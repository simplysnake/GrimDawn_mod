#pragma once
// Desktop mirror: the game window shows what you see in the headset (one eye, cropped to a normal field
// of view, with the HUD laid over it flat), so recording and streaming tools can capture the game window.

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace gdvr::mirror {

// Keeps a copy of the eye image now in the back buffer (call after that eye is rendered).
void KeepEye(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer);

// Draws the kept eye into the back buffer, then the HUD that is in it now on top (or no HUD).
// tanX/tanY: the eye image's half field of view (tangents); fovDeg: the mirror's horizontal field of view.
void Compose(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer, float tanX, float tanY,
             float fovDeg, bool withHud);

// Forgets the eye copy (e.g. a frame without stereo, so an old picture isn't shown).
void Invalidate();

void Shutdown();

}  // namespace gdvr::mirror
