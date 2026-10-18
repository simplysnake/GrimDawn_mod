# Grim Dawn VR

True-stereo VR for Grim Dawn: the game rendered once per eye from a camera that follows your head, the
HUD on a floating panel, the VR controllers as a laser pointer, mouse and keyboard, 3D damage numbers and
item names, and the Devotion constellations as a sky dome.

Unofficial; not affiliated with Crate Entertainment. Source available under the
[PolyForm Noncommercial License 1.0.0](LICENSE.md): free to use, change and share for non-commercial
purposes. Players: see [docs/INSTALL.md](docs/INSTALL.md) (it is the README of every release).

## Build

Needs Visual Studio 2022 or newer with the **Desktop development with C++** workload
(it includes CMake). Dependencies (MinHook, Dear ImGui, OpenXR SDK) are downloaded by CMake.

```
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
```

This is the development build, in `bin/`: it also logs to `logs/gdvr.log` in the repo and keeps
diagnostics. A release (`dist/GrimDawnVR-<version>/` and a zip) comes from

```
powershell -ExecutionPolicy Bypass -File tools\package.ps1
```

## Run

`bin\gdvr_launch.exe` (add it to Steam as a non-Steam game, Steam Input disabled for it). It finds the
game through Steam, puts the `dinput8.dll` stand-in in the game's `x64` folder (dormant unless the
launcher starts the game), backs up the saves, swaps in the VR copy of the game's `options.txt`
(`options_vr.txt`; the normal one is put back on exit), starts SteamVR and the game. The mod runs from a
copy in `%LOCALAPPDATA%\gdvr\run`, so it can be rebuilt while the game runs; `bin\gdvr_inject.exe` then
swaps the new build into the running game (hot reload, `--eject` unloads).

Log: `%LOCALAPPDATA%\gdvr\gdvr.log` (and `launcher.log`).

## Layout

| Path | What |
|---|---|
| `src/gdvr/engine_api.h` | Engine exports we use (mangled names + signatures) |
| `src/gdvr/engine_hooks.cpp` | Hook on `GraphicsMTRenderer::Render`, camera readout, experiments |
| `src/gdvr/overlay.cpp` | `IDXGISwapChain::Present` hook + ImGui |
| `src/injector/main.cpp` | Injector / hot reloader (development) |
| `src/launcher/main.cpp` | Launcher: finds the game, installs the stand-in, options swap, starts it |
| `src/proxy/dinput8.cpp` | `dinput8.dll` stand-in that loads the mod when the launcher starts the game |
| `tools/package.ps1` | Release build + package |
| `docs/INSTALL.md` | Player guide (the README of a release) |
| `tools/pe_dump.py` | Dumps a DLL's exports and imports |
| `notes/` | Findings (export dumps: `tools/pe_dump.py <dll> --exports notes/exports_<Module>.txt`, not in git) |

## VR

- Hotkeys: **F9** VR mode (Off → Screen → Stereo), **F10** recentre (or click both sticks), **F8** debug window,
  **F7** fade mask (on → distance rings → off), **F11** save both eye images to `%LOCALAPPDATA%\gdvr\eyeN.bmp`.
- Settings: `%LOCALAPPDATA%\gdvr\control.ini`, section `[vr]`: `mode`, `world_scale`, `fov_deg` (0 = fit lenses),
  `level_horizon`, `recompute_frustum`, `screen_distance`, `screen_width`, `recenter` (change the number to recentre),
  `far_plane` (default 70), `mask`, `mask_inner`/`mask_outer` (fade distances from the player, default 45/62),
  `mask_depth_start`, `mask_strength`, `mask_debug`, `mask_depth_pick`, `bench` (stereo while the headset sleeps).
  HUD panel: `hud`, `hud_distance`, `hud_width`, `hud_down`, `hud_tilt` (metres / degrees).
  Minimap + quest block (top right of the HUD) moved left: `hud_side_shift` (fraction of the HUD width),
  `hud_side_width` / `hud_side_height` (block size, fractions from the top-right corner);
  `hud_menu_coverage` (HUD coverage above which a menu counts as open and the block goes back).
  3D labels: `damage_numbers_3d`, `item_labels_3d` (drawn by the mod into each eye at the world position;
  the game's flat ones are hidden, so item names can't be clicked), `labels_in_world` (other world labels
  placed on the HUD panel along the line of sight). Camera: `camera_pull` (game units closer to the character).
- Development builds also write the log to `logs/gdvr.log` in the repo.
  HUD look (sliders in the F8 debug window, **Save settings** writes them here): `hud_brightness`, `hud_opacity`,
  `hud_alpha_boost` (makes dark HUD parts solid), dark backing behind the HUD for night and day:
  `hud_halo` / `hud_halo_radius` (night), `hud_halo_day` / `hud_halo_radius_day` (daylight), `hud_day_adapt`,
  `hud_day_from` / `hud_day_to` (scene brightness for night / full day; the log prints it every 5 s).
- **Controllers**: VR controllers act as a virtual gamepad (merged with any real gamepad) through the game's own
  gamepad mode, so turn on *Gamepad Support* in the game's options. Default Index layout: left stick move,
  right stick = right stick, right A/B = A/B, left A/B = X/Y, grips = bumpers, triggers = triggers, stick clicks,
  right trackpad press = Start, left trackpad press = Back. D-pad and Recenter are unbound by default.
  Remap everything in SteamVR: Settings > Controllers > Manage Controller Bindings > Grim Dawn VR.
  SteamVR's *Reset View* also recentres.
- While VR is on the game keeps running when its window is unfocused (SteamVR dashboard, other windows);
  keyboard and mouse still only reach the game when it is focused. `keep_running_unfocused=0` disables this.
- The game must run windowed with vsync off: the launcher's VR copy of the game's settings (`options_vr.txt`) has that.
