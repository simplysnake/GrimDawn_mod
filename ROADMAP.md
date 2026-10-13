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
| HUD | Game HUD captured to a floating, tilted panel (0.7 m) |
| Input | Index / Steam Controller / gamepad through a virtual XInput pad; OpenXR actions rebindable in SteamVR; recentre by clicking both sticks or SteamVR Reset View |
| Launching | One-click Steam shortcut (SteamVR → game → mod), save backup, hot reload |
| Tooling | Per-eye D3D11 trace, eye image dump (F11), memory scans, disassembler / symbol tools |

## Next (suggested order)

### 1. HUD readability in daylight – small (hours)
- Brightness and opacity settings for the HUD panel.
- Auto-dim from the average brightness of the eye images (more in daylight, none at night).
- Optional soft dark halo behind HUD elements so they read against bright ground.

### 2. Mouse in VR: world cursor – medium (1–2 weeks)
- Hide the game cursor; keep a cursor in the 3D world, clamped to the masked area around the player.
- Mouse deltas move it relative to the view; draw it as a 3D marker in both eyes.
- Each frame, project it through the game's own camera (`Camera::Project` / `GetImagePoint`) and feed that
  screen position to the game, so hover, highlight and click-to-move work unchanged.
- Over the HUD panel, switch to 2D panel coordinates (inventory, skills, menus).

### 3. Gaze cursor – small once 2 exists (~1 day)
- Cursor defaults to where the head points on the ground; mouse movement lets it drift off;
  after a short idle time it glides back to the gaze point.
- Same pipeline for a controller laser pointer.

### 4. Comfort – medium
- Stabilised world: smooth camera follow instead of locked to the character (motion-sickness risk).
- Live world-scale control (stick / mouse wheel) from tabletop to close-up.
- Seated and standing presets.

### 5. Constellation skybox – medium (1–2 weeks)
- No text: only constellation art, stars and the glow of completed constellations.
- Extract art from the user's own install on first run (never ship game assets).
- Constellation layout from the game database; completed state from the save file first, live memory later.
- Draw in the mask shader: "void" pixels sample the sky in their view direction instead of black.

### 6. Floating labels in the world – medium
- Monster health bars, damage numbers and item names currently land on the HUD panel at flat-screen
  positions; project them into the 3D scene instead (same projection idea as the world cursor).

### 7. Controllers beyond Index – small
- Default bindings for Quest 3 Touch Plus, Windows Mixed Reality, Pico, Vive Cosmos.
- Meta / Virtual Desktop native runtimes have no SteamVR binding UI: add an in-mod remap for them.
  (Through SteamVR, Meta controllers already work and are rebindable like Index.)

### 8. Haptics – small
- Route the game's controller rumble (hits, explosions) to the VR controllers.

### 9. Performance – ongoing
- Test in heavy combat (profiler logs automatically).
- Per-eye render-resolution scaling; tighter, asymmetric per-eye frusta.

### 10. In-VR settings panel – medium
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

### 11. Distribution (if shared) – medium
- Auto-detect game path; detect game version and fail safely after patches; one-click install.
- Stop sharing `options.txt` with vanilla play (VR settings currently change the normal game's settings).

## Known limitations

- The game pauses when its window is unfocused (`inactiveUpdateRate = 0`), freezing the headset.
- Native 90 Hz is unrealistic (~10 ms per frame is game logic/HUD); target 45 fps + SteamVR motion smoothing.
- Grass and rocks meet the ground with hard edges (engine soft blending is off in VR).
