# Findings

Game build: `Grim Dawn.exe` FileVersion 0.3.0.0, x64 (`G:\Games\Steam\steamapps\common\Grim Dawn\x64`).
Export dumps: `exports_*.txt` (`ordinal  RVA  mangled-name`), made with `tools/pe_dump.py`.

## Big result: the engine exports its C++ API by name

| DLL | Named exports |
|---|---|
| Engine.dll | 6,283 |
| Game.dll | 25,113 |
| Direct3D11.dll | 164 |

`Grim Dawn.exe` imports ~3,800 of these. So we can hook engine functions by name with
`GetProcAddress`: no signature scanning, and hooks survive most patches.

## Rendering

- `GraphicsMTRenderer::Render(const Camera&, Region*, const Viewport&, const Frustum&, uint, bool, const WorldFrustum*)`:
  **public**, takes the camera by reference. Main stereo candidate: call it once per eye.
- Internals: `RenderCamera`, `RenderScene`, `RenderFinalScene`, `RenderPostEffects`, `RenderDepth`,
  `BuildScene(bool, const WorldFrustum*, GraphicsScene&, vector<Region*>&)` (culling/scene build),
  `GetDepthBuffer`, `GetDepthTexture`, `CreateRenderTargets`.
- UI is drawn separately via `GraphicsCanvas` (`Display::Render`, `OverlayActivityManager::Render`, ...),
  so the 3D scene and UI should be separable.
- `GraphicsEngine::PresentSurface()`, `Engine::Render()` (top level), global `GAME::gEngine`.
- Direct3D11.dll wraps D3D in class `Direct3DDevice11`.

## Camera

- `Camera`: `GetCoords/SetCoords` (`Coords` = probably 3 axes + origin, 12 floats; to confirm),
  `GetFOV`, `GetNearPlane`, `GetFarPlane`, `GetCameraAspect`, `GetCameraToScreenMatrix(Mat4&, const Viewport&)`,
  `GetSubCameraToScreenMatrix` (off-centre projection? possibly usable for per-eye asymmetric frusta),
  `GetFrustum(const Viewport&[, float])`, `GetRayThroughImagePoint` (mouse picking).
- `WorldCamera`: `GetCameraPitch/Yaw/Distance`, `Set...`, `CalculateViewPosition`, `Update`, `GetRegionFrustum`.
- `GameCamera` (Game.dll): follows the player; `Update`, `UpdateTarget`, `UpdatePitch`, `Shake`, `Zoom`.
- `GraphicsScene`: `GetWorldToCameraMatrix`, `GetCameraToScreenMatrix`, `SetViewer`, `SetFrustum`.

## Week 1 results (2026-10-12, 3840x2160, Devil's Crossing area)

- **1 `Render` call per frame**, viewport = full window. No extra passes to filter out.
- **Camera**: FOV 0.5236 rad (30° vertical), near 1.0, far 120.0, aspect 1.0 (hmm: check what this field means).
- **`Coords` layout confirmed**: floats 0-2, 3-5, 6-8 = three unit axes (rotation); **9-11 = position** (Y is up).
  Floats 12-15 = 0.524 1 1 1 (FOV again + something; not part of Coords probably).
  Axis 0 = (-0.643, 0, 0.766) is horizontal → likely camera right; use it for the eye offset.
- **Offset experiment works**: 3 units sideways shifts the view cleanly, no artifacts or crash.
- **Culling confirmed**: 12 units sideways shows black holes where geometry outside the original frustum is skipped.
  Fix: widen the frustum passed to `Render` / `BuildScene`.
- **Render twice works**: stable; CPU cost of the 2nd pass ≈ 1.5 ms (same as the 1st). GPU cost not yet measured.
- **Hot reload works** while the game runs.
- **Input**: the game reads the mouse directly, so ImGui doesn't get clicks reliably. Experiments can be driven
  from `%LOCALAPPDATA%\gdvr\control.ini` (`[exp]` offset_enabled, origin_index, offset_x/y/z, target_call, render_twice).

## Week 2 results (2026-10-12): stereo in the headset works — GO

- OpenXR (SteamVR, Lighthouse HMD, RTX 5080) sharing the game's D3D11 device; eye images captured with
  CopyResource from the back buffer right after each `Render` call (scene is in the back buffer, pre-UI).
- `Coords` axis 0 points screen-**left** (projection X scale is negative). Mapping it as right mirrored yaw/roll
  and swapped the eyes; fixed by using -axis0 as right.
- `SetFOV` sets the **vertical** FOV. `SetCameraAspect` multiplies the horizontal extent (tanX grows with it),
  so the frustum can be fitted to the lens: Index-class lens needs tan 1.605 x 1.466 (~116° x 111°).
- Recomputing the frustum per eye (`Camera::GetFrustum`) is used for culling; user reported it working well.
- Performance at 2560x1440, both eyes: scene ~20 ms avg (7.5–43 ms) → ~50 fps before the rest of the frame.
  Performance is the main remaining work.
- Game must be windowed (fullscreen minimises when SteamVR takes focus) with vsync off.

## Performance (2026-10-12, 2560x1440, standing still in a town area)

Profiler: `perf:` lines in gdvr.log (CPU per stage via hooks on GraphicsMTRenderer internals, GPU via
timestamp queries). `[vr] bench=1` renders stereo while the headset sleeps; `tools/sweep.ps1` sweeps a setting.

- **CPU-bound.** GPU per-eye time ~= CPU per-eye time, so the GPU waits for draw calls.
- Stereo at far plane 120: 37 fps / 27 ms. `RenderScene` (draw submission) = 17.3 ms of it (both eyes);
  BuildScene (culling) 2.8, reflections ~2.6. Mono for comparison: RenderScene 2.5 ms.
- ~10 ms per frame is outside rendering (game logic, HUD, present), so native 90 Hz is unrealistic;
  target 45 fps + SteamVR motion smoothing.
- Far plane sweep: 120 -> 37.6 fps, 90 -> 42.6, **70 -> 49.0**, 50 -> 50.2. Default set to 70; the fade mask
  must hide the cut-off.
- Eye 0 consistently costs more than eye 1 (shared work done on the first pass).

## Fade mask (2026-10-12) - confirmed working in the headset

- Player position: `GraphicsMTRenderer::GetCameraTarget()` -> `WorldVec3` = `Region*` (8 bytes) + x, y, z in the
  same space as `Camera::GetCoords` (camera sits 20 units back along its forward from it). All zero in menus.
- The camera passed to `Render` is a plain `Camera` (WorldCamera getters return 0).
- Scene depth: not bound after `Render`; found by hooking `ID3D11DeviceContext::OMSetRenderTargets` (vtable 33)
  during each eye render. Each eye binds shadow maps (1536^2, 512^2, R16) and two screen-sized R24G8 depth buffers;
  the last screen-sized one bound is correct (verified with the distance-ring debug view, F7).
- Depth is standard (not reversed): z = n*f / (f - d*(f-n)).
- Defaults: clear inside 45 units from the player, black by 62, plus a fade over the last 15% of the far plane.
- Hot reload must tear down on the render thread (inside Present): doing it from the injector's thread raced the
  D3D11 immediate context and hung the game.

## Right eye missing glow / FXAA (2026-10-12) - fixed

- D3D trace per eye (`diag_trace`, tools: d3dtrace.cpp) showed the second render skipped the target-glow pass
  (clear, 9 draws of highlighted objects, 2560x720 + 1280x720 blur, composite) and two final full-screen passes.
- Cause: per-frame render settings at `GraphicsMTRenderer+0x1E0` (16 bytes: flag bytes + a float, 1.5) are set
  by the game each frame and cleared by the renderer after the first Render. Found with the "consumed" scan
  (`diag_toggle`): words changed by the first eye's render and untouched by the second.
- Fix: copy those 16 bytes before the first eye and restore them before the second. F5 toggles it.
- Not it: the Engine.dll frame counter (+0x41C574) - bumping it changed nothing (kept, off).

## Grass outlines in the second eye (2026-10-12) - open

- Eye dumps (F11 / dump_eyes -> %LOCALAPPDATA%\gdvr\eyeN.bmp): the right eye's grass is covered in dark
  streaks; everything else matches the left eye.
- Pass structure is identical per eye (D3D trace); the difference is in grass data.
- `GrassObject::SetupGrassGeometry` runs 0x/frame in mono, ~1.6-6x/frame in stereo (mostly in the first eye).
  Wide FOV = ~26 grass draws per eye vs 4 in mono, and the two eye cameras alternate, so grass geometry
  is rebuilt continuously; the second eye draws stale/partly rebuilt grass blocks.
- Not it: world frustum per eye (F4) - culls leaves/walls wrongly (frustum space mismatch), default off.
- No per-frame state consumed by the first eye in the engine DLLs' globals (diag_gconsumed).

## Dark edges on grass / rocks / characters (2026-10-12) - fixed (effect switched off)

- Not a separate pass: the geometry pass itself samples `ps slot 3` = a copy of the PREVIOUS render's depth
  (soft blending of grass, rock crevices, characters into the ground). The engine refreshes the copy
  (CopyResource G-buffer depth -> copy) only after the geometry pass. In stereo the previous render is the other
  eye -> misaligned dark edges that move with head rotation. Found by tracing screen-sized SRVs per pass.
- Per-eye history (each eye's own previous depth) aligned the edges but lagged head motion.
- Fix: clear the engine's depth copy to far depth before each eye's geometry pass (blend becomes a no-op; the copy
  is still refreshed for later passes). User: "off is the correct move, it looks good".
- Earlier theories that were wrong: grass churn (F2 keep-grass no effect), shadow matrix mismatch, frame counter,
  per-eye world frustum (culls wrongly), the fade mask, SteamVR motion smoothing.

## Week 1 questions (answer with the debug overlay)

1. How many `Render` calls per frame, and which is the main view? (viewport size)
2. Which `Coords` floats are the origin? Walk around; the changing triple is the position.
3. Does the offset experiment move the view cleanly, and what breaks (culling, shadows, fog)?
4. Does calling `Render` twice per frame work, and what does it cost?
5. In RenderDoc: which render target holds the finished scene before UI? Is depth reverse-Z?
