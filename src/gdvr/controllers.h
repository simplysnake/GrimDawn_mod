#pragma once
// Controller models in the headset: SteamVR's own render models (the OBJ + texture files it ships for each
// controller), drawn into each eye image at the tracked controllers.

namespace gdvr::controllers {

// Draws both controllers into the current eye image (the back buffer, before it is handed to the headset).
// tanX / tanY: the symmetric frustum the eye was rendered with.
void DrawEye(int eye, float tanX, float tanY);

void DrawDebugUI();  // F8: which model is loaded, where from
void Shutdown();

}  // namespace gdvr::controllers
