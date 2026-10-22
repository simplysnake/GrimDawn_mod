# Grim Dawn VR – Roadmap

True-stereo VR for Grim Dawn: a miniature world below you, head-tracked, faded to black at the edges,
with the devotion constellations as the sky.

## Done

| Area | What works |
|---|---|
| Stereo | Per-eye rendering through `GraphicsMTRenderer::Render`, head-tracked camera, FOV fitted to the lenses |
| Image correctness | Left/right axis fix; glow + FXAA in both eyes (per-frame settings replay); depth-history soft blend switched off (fixed dark edges on grass, rocks, characters and rock shadows) |
| Performance | Profiler (CPU per stage, GPU per eye); far plane 70 → ~45–60 fps, CPU-bound |
| Fade mask | World-space fade around the player from scene depth (F7, distance-ring debug view) |
| HUD | Game HUD captured to a floating, tilted panel; placement sliders (distance, height, size, tilt); brightness/opacity and a dark backing that strengthens in daylight; minimap + quest block can be moved left (goes back in place while a menu is open, menus draw on top) |
| Labels in 3D | Damage numbers (from the game's combat-text event) and item names (from the label layout) drawn by the mod into each eye at their world position, with real depth; item names keep rarity colours; health bars show |
| Camera | Viewpoint pulled towards the character (whole zoom range moves closer; F8 > World) |
| Interiors | Caves and interiors tested: look fine with the level-horizon view |
| Settings | F8 window: HUD placement/look, world, 3D labels, Save to control.ini; defaults = tested values, Stereo mode |
| Input | Index / Steam Controller / gamepad through a virtual XInput pad; OpenXR actions rebindable in SteamVR; recentre by clicking both sticks or SteamVR Reset View |
| Launching | One-click Steam shortcut (SteamVR → game → mod), save backup, hot reload |
| Focus | Game keeps running when its window loses focus (`WinWindow::IsActive` reports active in VR); keyboard/mouse stay focus-gated |
| Backup | Private GitHub repo: simplysnake/GrimDawn_mod |
| Tooling | Per-eye D3D11 trace, eye image dump (F11), memory scans, disassembler / symbol tools |

## Next (suggested order)

### 1. Pointer in VR: world cursor + controller laser – medium (1–2 weeks)
One pipeline: a point in the 3D world becomes the game's mouse position. Sources feed it the point:
- **Controller laser** (primary): a ray from the VR controller; trigger = click (move / attack / pick up),
  skills cast at the pointed spot. Works alongside stick movement (gamepad mode).
- **Mouse**: deltas move the point relative to the view, clamped to the masked area around the player.
- Each frame, project the point through the game's own camera and feed that screen position to the game,
  so hover, highlight, picking and click-to-move work unchanged; hide the game's flat cursor.
- Draw the laser and a 3D cursor marker in both eyes (same per-eye drawing as the 3D labels).
- Over the HUD panel: the laser hit becomes a panel position, i.e. a mouse position on the game's screen
  (inventory, stash, skills, menus become point-and-click).
- Check how the game mixes gamepad and mouse control (it switches on the last input device).

### 2. Gaze cursor – small once 1 exists (~1 day)
- A third pointer source: where the head points on the ground; after a short idle time the cursor glides
  back to the gaze point.

### 3. Comfort – medium
- Stabilised world: smooth camera follow instead of locked to the character (motion-sickness risk).
- Live world-scale control (stick / mouse wheel) from tabletop to close-up.
- Seated and standing presets.
- Decide how the game's own camera zoom (mouse wheel) and rotation behave alongside world scale and the
  level-horizon view.

### 4. Head-tracked 3D audio – small
- The game puts the audio listener at its own camera (`WorldCamera::SetListenerPosition`); move it to the
  head so sounds turn with you.

### 5. Screen-space effects – test + fixes
- Rain/snow overlays, hit flashes, blur ("screen effects" option) assume a flat screen; check each in
  stereo, fix or disable in VR.

### 6. Constellation skybox – medium (1–2 weeks)
- No text: only constellation art, stars and the glow of completed constellations.
- Extract art from the user's own install on first run (never ship game assets).
- Constellation layout from the game database; completed state from the save file first, live memory later.
- Draw in the mask shader: "void" pixels sample the sky in their view direction instead of black.

### 7. UI-heavy screens – usability pass
- Full-screen map (M) – ideally as a tabletop map; merchants/stash, dialogue, character select, the real
  devotion screen, video cutscenes (currently HUD panel or flat-screen fallback).

### 7b. Title screen characters in true 3D – medium (a few sessions)
- Today the character on the title / character-select screen is a flat picture: the game renders it from
  its 3D model, but in a separate render straight into the menu layer, so it only has one view.
- Find that render in the engine (not `GraphicsMTRenderer::Render`, which the mod already hooks: the frame
  log shows one world render per frame on the title screen) – trace the draws that produce the character.
- Render it once per eye with the same eye offsets as the world.
- Show each eye its own copy: two OpenXR quad layers with `eyeVisibility` left / right (or draw it into the
  eye images at a chosen distance), so the character stands in the menu scene with real depth.
- Keep the flat picture as the fallback if the render can't be found after a game patch.

### 8. Controllers beyond Index – small
- Default bindings for Quest 3 Touch Plus, Windows Mixed Reality, Pico, Vive Cosmos.
- Meta / Virtual Desktop native runtimes have no SteamVR binding UI: add an in-mod remap for them.
  (Through SteamVR, Meta controllers already work and are rebindable like Index.)

### 9. Haptics – small
- Route the game's controller rumble (hits, explosions) to the VR controllers.

### 10. Performance – ongoing
- Test in heavy combat (profiler logs automatically).
- Per-eye render-resolution scaling; tighter, asymmetric per-eye frusta.

### 11. In-VR settings panel – medium
- Adjust scale, mask, HUD and bindings with a controller instead of editing `control.ini`.
- **Per-element HUD placement** (sub-job):
  - Split the HUD into separate floating elements (health globe, energy globe, skill bar, minimap,
    quest list, buffs, open windows such as inventory) – each its own panel.
  - Per element: distance from the face (closer / further) and vertical offset (up / down).
  - Each element turns to face the user every frame (billboarded toward the head).
  - Implementation: all elements share the one captured HUD image; each is a separate OpenXR quad layer
    showing just its rectangle of it (`subImage.imageRect`), so no extra rendering. Element rectangles
    come from the game's UI layout (resolution / UI-scale dependent) – start with a fixed layout table,
    later read them from the game's UI widgets.
  - Settings saved per element; sensible defaults (e.g. globes low at the sides, minimap high on one side).

### 12. Robustness – small/medium
- Startup self-check of the hard-coded layouts found by experiment (per-frame settings at `renderer+0x1E0`,
  camera-target layout, camera axis order); disable the affected feature with a clear message if a game
  patch moved them.
- Graceful fallback to flat mode if SteamVR closes or crashes mid-game.
- Multiplayer check: confirm online co-op behaves normally with the mod.

### 13. Distribution (if shared) – medium
- Auto-detect game path; detect game version and fail safely after patches; one-click install.
- Stop sharing `options.txt` with vanilla play (VR settings currently change the normal game's settings).
- User guide: setup, hotkeys, troubleshooting.

## Known limitations

- Native 90 Hz is unrealistic (~10 ms per frame is game logic/HUD); target 45 fps + SteamVR motion smoothing.
- Grass and rocks meet the ground with hard edges (engine soft blending is off in VR).
