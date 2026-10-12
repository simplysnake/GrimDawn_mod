# Grim Dawn VR — proof of concept

Stereo VR for Grim Dawn: a camera that follows your head and the game rendered once per eye.
This is week 1 of the proof of concept: an in-game debug overlay that hooks the engine's
renderer and shows the live camera.

## Build

Needs Visual Studio 2022 or newer with the **Desktop development with C++** workload
(it includes CMake). Dependencies (MinHook, Dear ImGui) are downloaded by CMake.

```
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
```

Output: `bin/gdvr.dll` and `bin/gdvr_inject.exe`.

## Run

1. Start Grim Dawn from Steam (64-bit). Load a character in **single player, offline**. Back up `Documents\My Games\Grim Dawn\save` first.
2. Run `bin\gdvr_inject.exe`. The debug window appears; **F8** shows/hides it.
3. After rebuilding, run `gdvr_inject.exe` again: it unloads the old copy and loads the new one (hot reload).
   `gdvr_inject.exe --eject` unloads only.

Log: `%LOCALAPPDATA%\gdvr\gdvr.log`

## Layout

| Path | What |
|---|---|
| `src/gdvr/engine_api.h` | Engine exports we use (mangled names + signatures) |
| `src/gdvr/engine_hooks.cpp` | Hook on `GraphicsMTRenderer::Render`, camera readout, experiments |
| `src/gdvr/overlay.cpp` | `IDXGISwapChain::Present` hook + ImGui |
| `src/injector/main.cpp` | Injector / hot reloader |
| `tools/pe_dump.py` | Dumps a DLL's exports and imports |
| `notes/` | Findings and export dumps |

## VR

- Launch with `bin\gdvr_launch.exe` (or the Steam shortcut). It starts SteamVR first, then the game, then the mod.
- **F9** cycles VR mode (Off → Screen → Stereo), **F10** recentres, **F8** toggles the debug window,
  **F7** cycles the fade mask (on → distance rings → off).
- Settings: `%LOCALAPPDATA%\gdvr\control.ini`, section `[vr]`: `mode`, `world_scale`, `fov_deg` (0 = fit lenses),
  `level_horizon`, `recompute_frustum`, `screen_distance`, `screen_width`, `recenter` (change the number to recentre),
  `far_plane` (default 70), `mask`, `mask_inner`/`mask_outer` (fade distances from the player, default 45/62),
  `mask_depth_start`, `mask_strength`, `mask_debug`, `mask_depth_pick`, `bench` (stereo while the headset sleeps).
  HUD panel: `hud`, `hud_distance`, `hud_width`, `hud_down`, `hud_tilt` (metres / degrees).
- The game must run windowed with vsync off: `tools\apply_vr_settings.ps1` applies recommended settings after the game exits.
