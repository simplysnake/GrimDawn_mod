#pragma once
// Controller input. VR controllers are read through OpenXR actions (rebindable in SteamVR's
// "Manage Controller Bindings") and turned into a virtual Xbox-style gamepad, which is merged
// into the game's own XInputGetState calls together with any real gamepad. The game then plays
// in its native gamepad mode.

#include <windows.h>

#include <Xinput.h>
#include <openxr/openxr.h>

namespace gdvr::input {

// Hooks XInputGetState in the game's XInput DLL (hook created, not enabled).
bool InstallXInputHook();

// Keeps the game from installing its low-level keyboard hook (which freezes the PC's keyboard
// whenever the game hangs) and removes it if it is already in place.
bool InstallKeyboardGuard();

// Creates the action set, actions and suggested bindings and attaches them to the session.
// Call once per session, before the first frame.
bool CreateActions(XrInstance instance, XrSession session);

// Reads the actions (call once per frame). Returns true if the "recenter" action was pressed.
bool Sync(XrSession session, bool focused);

// Controller aim pose (the pointer ray: -Z forward) for hand 0 = left, 1 = right, in `base`.
bool LocateAim(int hand, XrSpace base, XrTime time, XrPosef& pose);

// Pointer mode: the VR controls (as an Xbox-style pad) for the pointer module to turn into mouse
// and keyboard input. Returns false when the controls go to the game as a gamepad instead.
bool PointerControls(XINPUT_GAMEPAD& controls);

// Forgets the action set (call before the session/instance are destroyed).
void DestroyActions();

// Logs the interaction profile now in use for each hand.
void OnInteractionProfileChanged(XrInstance instance, XrSession session);

int InFlight();
void DrawDebugUI();

}  // namespace gdvr::input
