#pragma once
// Controller vibration. Grim Dawn has no rumble of its own (it never calls XInputSetState), so the mod
// makes it from what it can see: the player's life (hits taken, big heals, death) and the combat-text
// event (the player's hits, crits stronger).

namespace gdvr::haptics {

// A combat-text event (a hit the player or a pet dealt). Any thread.
void OnHit(bool crit);

// Once per frame on the render thread, before the controllers are synced: reads the player's life and
// queues the pulses (input::Vibrate).
void Update();

void DrawDebugUI();

}  // namespace gdvr::haptics
